#!/usr/bin/env python3
"""Systerian Translator — LithoKraft certificate-signing glyphs as a desktop tool.

Type any message; every character renders as the Systerian (Cistercian) glyph
of its own code point (the certificate-signing ASCII rule: 'A' = 65 -> glyph 65).
Geometry is the reference construction from generate.py: 9 unit strokes +
place transforms (tens mirror-x, hundreds flip-y, thousands both), JD's rivet
rule (a dot at every stroke-graph vertex, deduped within 0.5), halo outline
pass under every stroke and rivet.

Full-featured: message text, output dimensions with automatic wrap + fit,
9-point alignment (left/center/right x top/middle/bottom), line / rivet /
halo / background colors, stroke width, rivet radius, halo width, locked
colorway presets, export to SVG (vector) and PNG (raster, optional
transparent background). Pure standard library — no dependencies.

Run:  python systerian_translator.py            (GUI)
CLI:  python systerian_translator.py --text "HELLO" --out hello.png \
          --width 1200 --height 675 [--format svg] [style options]

Part of the LithoLattice engine package (tools/). CONFIDENTIAL — Wet Wafer Works.

Credit where it is due: the glyphs themselves belong to history — the
numeral system of the Cistercian monks, c. 1300, who carried knowledge
forward with them. LithoKraft borrows their signs with respect; Systerian
is our name for their script at the certificate table, not a claim on it.
"""
import math, os, struct, sys, zlib

# --------------------------------------------------------------------------
# Reference glyph geometry (identical construction to generate.py)
# --------------------------------------------------------------------------
SW_DEFAULT = 6.0        # stroke width, glyph units (100x140 grid)
DOT_R_DEFAULT = 5.4     # rivet radius
HALO_W_DEFAULT = 2.0    # halo outline width
CELL_W, CELL_H = 100.0, 140.0
STAVE = ((50.0, 10.0), (50.0, 130.0))

GAP_X = 16.0            # horizontal gap between glyph cells (engine text rule)
GAP_Y = 32.0            # vertical gap between rows

U = {  # unit strokes (upper right), segment lists
    1: [((50, 10), (80, 10))],
    2: [((50, 50), (80, 50))],
    3: [((50, 10), (80, 40))],
    4: [((50, 50), (80, 20))],
    5: [((50, 10), (80, 10)), ((80, 10), (50, 40))],
    6: [((78, 12), (78, 42))],
    7: [((50, 10), (80, 10)), ((80, 10), (80, 40))],
    8: [((50, 50), (80, 50)), ((80, 50), (80, 80))],
    9: [((50, 10), (80, 10)), ((80, 10), (80, 40)), ((80, 40), (50, 40))],
}


def _mx(seg):
    return ((100 - seg[0][0], seg[0][1]), (100 - seg[1][0], seg[1][1]))


def _fy(seg):
    return ((seg[0][0], 140 - seg[0][1]), (seg[1][0], 140 - seg[1][1]))


def _place_segments(place, digit):
    segs = [tuple(map(tuple, s)) for s in U[digit]]
    if place in ("tens", "thousands"):
        segs = [_mx(s) for s in segs]
    if place in ("hundreds", "thousands"):
        segs = [_fy(s) for s in segs]
    return segs


def glyph_segments_for_value(value):
    """Composed segment list (stave + up to 4 digit strokes) for 0..9999.
    0 = bare stave (the codec's zero convention)."""
    v = int(value) % 10000
    segs = [STAVE]
    thou, rem = divmod(v, 1000)
    hund, rem = divmod(rem, 100)
    ten, one = divmod(rem, 10)
    if one:
        segs += _place_segments("units", one)
    if ten:
        segs += _place_segments("tens", ten)
    if hund:
        segs += _place_segments("hundreds", hund)
    if thou:
        segs += _place_segments("thousands", thou)
    return segs


_EPS = 1e-9


def _sub(p, q):
    return (p[0] - q[0], p[1] - q[1])


def _cross(a, b):
    return a[0] * b[1] - a[1] * b[0]


def _seg_intersection(s1, s2):
    p, r = s1[0], _sub(s1[1], s1[0])
    q, s = s2[0], _sub(s2[1], s2[0])
    denom = _cross(r, s)
    if abs(denom) < _EPS:
        return None
    qp = _sub(q, p)
    t = _cross(qp, s) / denom
    u = _cross(qp, r) / denom
    if -_EPS <= t <= 1 + _EPS and -_EPS <= u <= 1 + _EPS:
        return (p[0] + t * r[0], p[1] + t * r[1])
    return None


def glyph_vertices(segs):
    """Every stroke-graph vertex: endpoints + crossings, deduped within 0.5."""
    pts = []
    for s in segs:
        pts += [s[0], s[1]]
    for i in range(len(segs)):
        for j in range(i + 1, len(segs)):
            p = _seg_intersection(segs[i], segs[j])
            if p:
                pts.append(p)
    uniq = []
    for p in pts:
        if not any(math.hypot(p[0] - q[0], p[1] - q[1]) < 0.5 for q in uniq):
            uniq.append(p)
    return uniq


def char_value(ch):
    """ASCII rule: a character maps to the glyph of its own code point
    (values taken mod 10000, per the engine codec)."""
    return ord(ch) % 10000


# --------------------------------------------------------------------------
# Style
# --------------------------------------------------------------------------
class Style:
    def __init__(self, line="#A970FF", dot="#FF6F00", halo="#FF3B30",
                 background="#0A0A12", stroke_width=SW_DEFAULT,
                 dot_radius=DOT_R_DEFAULT, halo_width=HALO_W_DEFAULT,
                 halo_enabled=True, transparent=False):
        self.line = line
        self.dot = dot
        self.halo = halo
        self.background = background
        self.stroke_width = float(stroke_width)
        self.dot_radius = float(dot_radius)
        self.halo_width = float(halo_width)
        self.halo_enabled = bool(halo_enabled)
        self.transparent = bool(transparent)

    def copy(self):
        return Style(self.line, self.dot, self.halo, self.background,
                     self.stroke_width, self.dot_radius, self.halo_width,
                     self.halo_enabled, self.transparent)


PRESETS = {
    "Violet on Dark (locked primary)":
        Style("#A970FF", "#FF6F00", "#FF3B30", "#0A0A12"),
    "Violet on Light":
        Style("#A970FF", "#FF6F00", "#2F7CF6", "#F4F1EA"),
    "White on Dark":
        Style("#FFFFFF", "#FF6F00", "#FF3B30", "#0A0A12"),
    "White on Light":
        Style("#FFFFFF", "#FF6F00", "#2F7CF6", "#F4F1EA"),
    "No Halo — Violet on Dark":
        Style("#A970FF", "#FF6F00", "#FF3B30", "#0A0A12", halo_enabled=False),
    "No Halo — Transparent":
        Style("#A970FF", "#FF6F00", "#FF3B30", "#0A0A12",
              halo_enabled=False, transparent=True),
}


def hex_to_rgb(h):
    h = h.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


# --------------------------------------------------------------------------
# Layout: wrap + auto-fit + alignment
# --------------------------------------------------------------------------
def layout_text(text, box_w, box_h, halign="center", valign="middle",
                fit="lines"):
    """Returns (placements, scale): placements = list of (value, x_px, y_px)
    glyph-cell origins in output pixels; scale = px per glyph unit.

    Fit modes:
      "lines" — logical lines (split on newlines) are kept intact and the
                block scales down to fit the box (banner behaviour).
      "fill"  — lines also wrap at a searched column count so the glyphs
                come out as large as possible inside the box.
    The block is aligned per halign / valign, and each row follows halign
    within the block."""
    logical = text.split("\n")
    longest = max((len(l) for l in logical), default=0)
    if longest == 0 or box_w <= 0 or box_h <= 0:
        return [], 1.0
    advance = CELL_W + GAP_X
    pitch = CELL_H + GAP_Y

    best = None  # (scale, rows) with rows = list of strings
    col_range = range(longest, longest + 1) if fit == "lines" \
        else range(1, longest + 1)
    for cols in col_range:
        rows = []
        for line in logical:
            if line == "":
                rows.append("")
            else:
                for i in range(0, len(line), cols):
                    rows.append(line[i:i + cols])
        widest = max(len(r) for r in rows)
        content_w = widest * advance - GAP_X if widest else 0.0
        content_h = len(rows) * pitch - GAP_Y
        if content_w <= 0 or content_h <= 0:
            continue
        s = min(box_w / content_w, box_h / content_h)
        if best is None or s > best[0] + 1e-12:
            best = (s, rows)
    if best is None:
        return [], 1.0
    scale, rows = best
    widest = max(len(r) for r in rows)
    block_w = (widest * advance - GAP_X) * scale if widest else 0.0
    block_h = (len(rows) * pitch - GAP_Y) * scale
    if halign == "left":
        block_x = 0.0
    elif halign == "right":
        block_x = box_w - block_w
    else:
        block_x = (box_w - block_w) / 2.0
    if valign == "top":
        block_y = 0.0
    elif valign == "bottom":
        block_y = box_h - block_h
    else:
        block_y = (box_h - block_h) / 2.0

    placements = []
    for ri, row in enumerate(rows):
        row_w = (len(row) * advance - GAP_X) * scale if row else 0.0
        if halign == "left":
            row_x = block_x
        elif halign == "right":
            row_x = block_x + (block_w - row_w)
        else:
            row_x = block_x + (block_w - row_w) / 2.0
        y = block_y + ri * pitch * scale
        for ci, ch in enumerate(row):
            x = row_x + ci * advance * scale
            placements.append((char_value(ch), x, y))
    return placements, scale


# --------------------------------------------------------------------------
# SVG rendering
# --------------------------------------------------------------------------
def _glyph_svg_body(value, style):
    """One glyph's SVG elements in its own 100x140 cell space (no <svg>)."""
    segs = glyph_segments_for_value(value)
    verts = glyph_vertices(segs)
    d = "".join(f"M{s[0][0]} {s[0][1]}L{s[1][0]} {s[1][1]}" for s in segs)
    parts = []
    if style.halo_enabled and style.halo_width > 0:
        hw = style.halo_width
        parts.append(
            f'<path d="{d}" fill="none" stroke="{style.halo}" '
            f'stroke-width="{style.stroke_width + 2 * hw}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>')
        for p in verts:
            parts.append(f'<circle cx="{p[0]:.2f}" cy="{p[1]:.2f}" '
                         f'r="{style.dot_radius + hw}" fill="{style.halo}"/>')
    parts.append(
        f'<path d="{d}" fill="none" stroke="{style.line}" '
        f'stroke-width="{style.stroke_width}" stroke-linecap="round" '
        f'stroke-linejoin="round"/>')
    for p in verts:
        parts.append(f'<circle cx="{p[0]:.2f}" cy="{p[1]:.2f}" '
                     f'r="{style.dot_radius}" fill="{style.dot}"/>')
    return "".join(parts)


def render_svg(text, width, height, style, halign="center", valign="middle",
               fit="lines"):
    placements, scale = layout_text(text, width, height, halign, valign, fit)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
             f'height="{height}" viewBox="0 0 {width} {height}">']
    if not style.transparent:
        parts.append(f'<rect width="{width}" height="{height}" '
                     f'fill="{style.background}"/>')
    for value, x, y in placements:
        parts.append(f'<g transform="translate({x:.2f} {y:.2f}) '
                     f'scale({scale:.6f})">{_glyph_svg_body(value, style)}</g>')
    parts.append("</svg>")
    return "".join(parts)


# --------------------------------------------------------------------------
# PNG rendering: ink-stamp rasterizer (supersampled) + pure-python encoder
# --------------------------------------------------------------------------
SS = 2  # supersample factor for anti-aliasing (stamps carry analytic
        # distance coverage, so 2x plus the falloff gives clean edges)


def _blend(buf, stride, px, py, rgb, cov):
    if cov <= 0.0:
        return
    i = (py * stride + px) * 4
    a = cov
    inv = 1.0 - a
    buf[i] = int(rgb[0] * a + buf[i] * inv + 0.5)
    buf[i + 1] = int(rgb[1] * a + buf[i + 1] * inv + 0.5)
    buf[i + 2] = int(rgb[2] * a + buf[i + 2] * inv + 0.5)
    buf[i + 3] = int(255 * a + buf[i + 3] * inv + 0.5)


def _stamp_segment(buf, w, h, p, q, radius, rgb):
    x0 = max(0, int(min(p[0], q[0]) - radius - 1))
    x1 = min(w - 1, int(max(p[0], q[0]) + radius + 1))
    y0 = max(0, int(min(p[1], q[1]) - radius - 1))
    y1 = min(h - 1, int(max(p[1], q[1]) + radius + 1))
    dx, dy = q[0] - p[0], q[1] - p[1]
    len2 = dx * dx + dy * dy
    for py in range(y0, y1 + 1):
        cy = py + 0.5
        for px in range(x0, x1 + 1):
            cx = px + 0.5
            if len2 == 0:
                dist = math.hypot(cx - p[0], cy - p[1])
            else:
                t = ((cx - p[0]) * dx + (cy - p[1]) * dy) / len2
                t = 0.0 if t < 0 else (1.0 if t > 1 else t)
                dist = math.hypot(cx - (p[0] + t * dx), cy - (p[1] + t * dy))
            cov = radius + 0.5 - dist
            if cov > 0:
                _blend(buf, w, px, py, rgb, min(1.0, cov))


def _stamp_dot(buf, w, h, c, radius, rgb):
    x0 = max(0, int(c[0] - radius - 1))
    x1 = min(w - 1, int(c[0] + radius + 1))
    y0 = max(0, int(c[1] - radius - 1))
    y1 = min(h - 1, int(c[1] + radius + 1))
    for py in range(y0, y1 + 1):
        for px in range(x0, x1 + 1):
            dist = math.hypot(px + 0.5 - c[0], py + 0.5 - c[1])
            cov = radius + 0.5 - dist
            if cov > 0:
                _blend(buf, w, px, py, rgb, min(1.0, cov))


def render_rgba(text, width, height, style, halign="center", valign="middle",
                ss_factor=None, fit="lines"):
    """Returns (width, height, bytearray RGBA)."""
    S = SS if ss_factor is None else ss_factor
    w, h = width * S, height * S
    buf = bytearray(w * h * 4)
    if not style.transparent:
        r, g, b = hex_to_rgb(style.background)
        for i in range(0, len(buf), 4):
            buf[i], buf[i + 1], buf[i + 2], buf[i + 3] = r, g, b, 255
    placements, scale = layout_text(text, width, height, halign, valign, fit)
    ss = scale * S
    line_rgb = hex_to_rgb(style.line)
    dot_rgb = hex_to_rgb(style.dot)
    halo_rgb = hex_to_rgb(style.halo)

    def draw(pass_halo):
        for value, ox, oy in placements:
            segs = glyph_segments_for_value(value)
            verts = glyph_vertices(segs)
            sw = style.stroke_width + (2 * style.halo_width if pass_halo else 0)
            dr = style.dot_radius + (style.halo_width if pass_halo else 0)
            rgb = halo_rgb if pass_halo else line_rgb
            for s in segs:
                p = (ox * S + s[0][0] * ss, oy * S + s[0][1] * ss)
                q = (ox * S + s[1][0] * ss, oy * S + s[1][1] * ss)
                _stamp_segment(buf, w, h, p, q, sw * ss / 2.0, rgb)
            for v in verts:
                c = (ox * S + v[0] * ss, oy * S + v[1] * ss)
                _stamp_dot(buf, w, h, c, dr * ss,
                           halo_rgb if pass_halo else dot_rgb)

    if style.halo_enabled and style.halo_width > 0:
        draw(True)
    draw(False)

    if S == 1:
        return width, height, buf
    out = bytearray(width * height * 4)
    n = S * S
    px = list(buf)  # one C-speed conversion; slice sums below run in C
    rowlen = w * 4
    for y in range(height):
        rows = []
        for sy in range(S):
            base = (y * S + sy) * rowlen
            rows.append(px[base:base + rowlen])
        for x in range(width):
            o = (y * width + x) * 4
            for c in range(4):
                tot = 0
                for r in rows:
                    seg = r[x * S * 4 + c:(x * S + S) * 4 + c:4]
                    tot += sum(seg)
                out[o + c] = tot // n
    return width, height, out


def png_bytes(width, height, rgba):
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def render_png(text, width, height, style, halign="center", valign="middle",
               ss_factor=None, fit="lines"):
    w, h, rgba = render_rgba(text, width, height, style, halign, valign,
                             ss_factor=ss_factor, fit=fit)
    return png_bytes(w, h, rgba)


# --------------------------------------------------------------------------
# CLI (headless rendering — also used for testing and scripting)
# --------------------------------------------------------------------------
def cli(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="systerian_translator",
                                 description="Systerian character translator")
    ap.add_argument("--text", required=True, help="message to translate")
    ap.add_argument("--out", required=True, help="output file (.png or .svg)")
    ap.add_argument("--format", choices=["png", "svg"], default=None)
    ap.add_argument("--width", type=int, default=1200)
    ap.add_argument("--height", type=int, default=675)
    ap.add_argument("--preset", choices=list(PRESETS), default=None)
    ap.add_argument("--line", default=None)
    ap.add_argument("--dot", default=None)
    ap.add_argument("--halo", default=None)
    ap.add_argument("--background", default=None)
    ap.add_argument("--stroke-width", type=float, default=None)
    ap.add_argument("--dot-radius", type=float, default=None)
    ap.add_argument("--halo-width", type=float, default=None)
    ap.add_argument("--no-halo", action="store_true")
    ap.add_argument("--transparent", action="store_true")
    ap.add_argument("--halign", choices=["left", "center", "right"],
                    default="center")
    ap.add_argument("--fit", choices=["lines", "fill"], default="lines",
                    help="lines: keep lines intact, shrink to fit; "
                         "fill: wrap to make glyphs as large as possible")
    ap.add_argument("--valign", choices=["top", "middle", "bottom"],
                    default="middle")
    a = ap.parse_args(argv)
    style = PRESETS[a.preset].copy() if a.preset else Style()
    if a.line:
        style.line = a.line
    if a.dot:
        style.dot = a.dot
    if a.halo:
        style.halo = a.halo
    if a.background:
        style.background = a.background
        style.transparent = False
    if a.stroke_width is not None:
        style.stroke_width = a.stroke_width
    if a.dot_radius is not None:
        style.dot_radius = a.dot_radius
    if a.halo_width is not None:
        style.halo_width = a.halo_width
    if a.no_halo:
        style.halo_enabled = False
    if a.transparent:
        style.transparent = True
    fmt = a.format or ("svg" if a.out.lower().endswith(".svg") else "png")
    text = a.text.replace("\\n", "\n")
    if fmt == "svg":
        data = render_svg(text, a.width, a.height, style,
                          a.halign, a.valign, fit=a.fit).encode()
    else:
        data = render_png(text, a.width, a.height, style, a.halign, a.valign,
                          fit=a.fit)
    with open(a.out, "wb") as f:
        f.write(data)
    print(f"wrote {a.out} ({fmt}, {a.width}x{a.height}, {len(data)} bytes)")
    return 0


# --------------------------------------------------------------------------
# GUI (tkinter, standard library)
# --------------------------------------------------------------------------
def run_gui():
    import tkinter as tk
    from tkinter import ttk, colorchooser, filedialog, messagebox

    root = tk.Tk()
    root.title("Systerian Translator — LithoKraft Certificate Signing")
    root.geometry("1180x760")
    root.minsize(900, 600)

    style = PRESETS["Violet on Dark (locked primary)"].copy()
    state = {"photo": None, "job": None}

    # --- left column: message + output -----------------------------------
    left = ttk.Frame(root, padding=10)
    left.pack(side="left", fill="y")
    ttk.Label(left, text="Message (each character = the glyph of its code)",
              font=("TkDefaultFont", 10, "bold")).pack(anchor="w")
    text_box = tk.Text(left, width=44, height=6, wrap="word",
                       font=("Consolas", 12))
    text_box.insert("1.0", "SYSTERIAN")
    text_box.pack(fill="x", pady=(2, 10))

    out_frame = ttk.LabelFrame(left, text="Output", padding=8)
    out_frame.pack(fill="x", pady=(0, 10))
    dims = ttk.Frame(out_frame)
    dims.pack(fill="x")
    ttk.Label(dims, text="Width (px)").grid(row=0, column=0, sticky="w")
    width_var = tk.StringVar(value="1200")
    ttk.Entry(dims, textvariable=width_var, width=8).grid(row=0, column=1,
                                                         padx=4)
    ttk.Label(dims, text="Height (px)").grid(row=0, column=2, sticky="w")
    height_var = tk.StringVar(value="675")
    ttk.Entry(dims, textvariable=height_var, width=8).grid(row=0, column=3,
                                                          padx=4)
    fit_row = ttk.Frame(out_frame)
    fit_row.pack(fill="x", pady=(6, 0))
    ttk.Label(fit_row, text="Fit").pack(side="left")
    fit_var = tk.StringVar(value="lines")
    ttk.Combobox(fit_row, textvariable=fit_var, state="readonly", width=10,
                 values=["lines", "fill"]).pack(side="left", padx=4)
    ttk.Label(out_frame, wraplength=380,
              text="lines: your lines stay intact and shrink to fit. "
                   "fill: text wraps so the glyphs grow as large as "
                   "possible.").pack(anchor="w", pady=(4, 0))

    al_frame = ttk.LabelFrame(left, text="Alignment", padding=8)
    al_frame.pack(fill="x", pady=(0, 10))
    halign_var = tk.StringVar(value="center")
    valign_var = tk.StringVar(value="middle")
    ttk.Label(al_frame, text="Horizontal").grid(row=0, column=0, sticky="w")
    for i, (lab, val) in enumerate([("Left", "left"), ("Center", "center"),
                                    ("Right", "right")]):
        ttk.Radiobutton(al_frame, text=lab, value=val,
                        variable=halign_var).grid(row=0, column=1 + i)
    ttk.Label(al_frame, text="Vertical").grid(row=1, column=0, sticky="w")
    for i, (lab, val) in enumerate([("Top", "top"), ("Middle", "middle"),
                                    ("Bottom", "bottom")]):
        ttk.Radiobutton(al_frame, text=lab, value=val,
                        variable=valign_var).grid(row=1, column=1 + i)

    btns = ttk.Frame(left)
    btns.pack(fill="x")
    status = tk.StringVar(value="Ready.")

    def current_style():
        return style

    def dims_or_none():
        try:
            w = int(width_var.get())
            h = int(height_var.get())
            if w < 20 or h < 20 or w > 8000 or h > 8000:
                raise ValueError
            return w, h
        except ValueError:
            status.set("Dimensions must be whole numbers, 20..8000 px.")
            return None

    def do_export(fmt):
        d = dims_or_none()
        if not d:
            return
        text = text_box.get("1.0", "end-1c")
        path = filedialog.asksaveasfilename(
            defaultextension="." + fmt,
            filetypes=[(fmt.upper() + " files", "*." + fmt)],
            initialfile="systerian." + fmt,
            title="Export " + fmt.upper())
        if not path:
            return
        try:
            if fmt == "svg":
                data = render_svg(text, d[0], d[1], current_style(),
                                  halign_var.get(), valign_var.get(),
                                  fit=fit_var.get()).encode()
            else:
                data = render_png(text, d[0], d[1], current_style(),
                                  halign_var.get(), valign_var.get(),
                                  fit=fit_var.get())
            with open(path, "wb") as f:
                f.write(data)
            status.set(f"Exported {os.path.basename(path)} "
                       f"({d[0]}x{d[1]}, {len(data)} bytes)")
        except Exception as e:  # never die on a bad export path
            messagebox.showerror("Export failed", str(e))

    ttk.Button(btns, text="Export SVG",
               command=lambda: do_export("svg")).pack(side="left", padx=(0, 6))
    ttk.Button(btns, text="Export PNG",
               command=lambda: do_export("png")).pack(side="left")
    ttk.Label(left, textvariable=status, wraplength=380,
              foreground="#666").pack(anchor="w", pady=(10, 0))
    ttk.Label(left, wraplength=380, foreground="#888",
              text="Glyphs: the Cistercian monks, c. 1300 — carried "
                   "forward, credited, and borrowed with respect.").pack(
                       anchor="w", pady=(8, 0))

    # --- right column: style + preview ---------------------------------
    right = ttk.Frame(root, padding=10)
    right.pack(side="left", fill="both", expand=True)

    st_frame = ttk.LabelFrame(right, text="Style", padding=8)
    st_frame.pack(fill="x")
    ttk.Label(st_frame, text="Preset").grid(row=0, column=0, sticky="w")
    preset_var = tk.StringVar(value="Violet on Dark (locked primary)")
    preset_box = ttk.Combobox(st_frame, textvariable=preset_var,
                              values=list(PRESETS), state="readonly",
                              width=34)
    preset_box.grid(row=0, column=1, columnspan=3, sticky="w", pady=(0, 6))

    color_vars = {}

    def color_row(row, label, attr):
        ttk.Label(st_frame, text=label).grid(row=row, column=0, sticky="w")
        var = tk.StringVar(value=getattr(style, attr))
        color_vars[attr] = var
        ent = ttk.Entry(st_frame, textvariable=var, width=10)
        ent.grid(row=row, column=1, sticky="w")
        swatch = tk.Label(st_frame, width=3, relief="solid",
                          background=var.get())
        swatch.grid(row=row, column=2, padx=4)

        def refresh(*_):
            raw = var.get().strip()
            try:
                hex_to_rgb(raw)
                normalized = raw.upper()
                if not normalized.startswith("#"):
                    normalized = "#" + normalized
            except Exception:
                try:  # accept Tk colour names too ("navy", "purple", ...)
                    r, g, b = (c >> 8 for c in root.winfo_rgb(raw))
                    normalized = f"#{r:02X}{g:02X}{b:02X}"
                except Exception:
                    ent.configure(foreground="#C00000")  # not a colour: say so
                    return
            ent.configure(foreground="#000000")
            swatch.configure(background=normalized)
            setattr(style, attr, normalized)
            schedule_preview()

        def pick():
            c = colorchooser.askcolor(color=var.get(),
                                      title="Choose " + label)
            if c and c[1]:
                var.set(c[1].upper())

        var.trace_add("write", refresh)
        ttk.Button(st_frame, text="Pick…", command=pick).grid(row=row,
                                                              column=3)
        return var

    color_row(1, "Line color", "line")
    color_row(2, "Dot (rivet) color", "dot")
    color_row(3, "Halo / outline color", "halo")
    color_row(4, "Background color", "background")

    def num_row(row, label, attr, lo, hi):
        ttk.Label(st_frame, text=label).grid(row=row, column=0, sticky="w")
        var = tk.DoubleVar(value=getattr(style, attr))
        val_lab = ttk.Label(st_frame, width=6)
        val_lab.grid(row=row, column=2)

        def on_move(*_):
            setattr(style, attr, float(var.get()))
            val_lab.configure(text=f"{var.get():.1f}")
            schedule_preview()

        var.trace_add("write", on_move)
        tk.Scale(st_frame, variable=var, from_=lo, to=hi, resolution=0.2,
                 orient="horizontal", length=180, showvalue=False,
                 command=lambda _v: None).grid(row=row, column=1, sticky="w")
        val_lab.configure(text=f"{var.get():.1f}")
        return var

    stroke_var = num_row(5, "Stroke width", "stroke_width", 1.0, 20.0)
    dot_var = num_row(6, "Dot radius", "dot_radius", 0.0, 15.0)
    halo_var = num_row(7, "Halo width", "halo_width", 0.0, 8.0)

    toggles = ttk.Frame(st_frame)
    toggles.grid(row=8, column=0, columnspan=4, sticky="w", pady=(4, 0))
    halo_on = tk.BooleanVar(value=style.halo_enabled)
    transp = tk.BooleanVar(value=style.transparent)

    def on_toggles(*_):
        style.halo_enabled = bool(halo_on.get())
        style.transparent = bool(transp.get())
        schedule_preview()

    halo_on.trace_add("write", on_toggles)
    transp.trace_add("write", on_toggles)
    ttk.Checkbutton(toggles, text="Halo outline on",
                    variable=halo_on).pack(side="left")
    ttk.Checkbutton(toggles, text="Transparent background",
                    variable=transp).pack(side="left", padx=(12, 0))

    def apply_preset(*_):
        p = PRESETS[preset_var.get()].copy()
        style.line, style.dot, style.halo = p.line, p.dot, p.halo
        style.background = p.background
        style.stroke_width = p.stroke_width
        style.dot_radius = p.dot_radius
        style.halo_width = p.halo_width
        style.halo_enabled = p.halo_enabled
        style.transparent = p.transparent
        for attr, var in color_vars.items():
            var.set(getattr(style, attr))
        stroke_var.set(style.stroke_width)
        dot_var.set(style.dot_radius)
        halo_var.set(style.halo_width)
        halo_on.set(style.halo_enabled)
        transp.set(style.transparent)
        schedule_preview()

    preset_var.trace_add("write", apply_preset)

    pv_frame = ttk.LabelFrame(right, text="Preview", padding=6)
    pv_frame.pack(fill="both", expand=True, pady=(10, 0))
    preview = ttk.Label(pv_frame, anchor="center")
    preview.pack(fill="both", expand=True)

    def refresh_preview():
        state["job"] = None
        try:
            w = max(60, preview.winfo_width())
            h = max(60, preview.winfo_height())
        except Exception:
            w, h = 640, 420
        text = text_box.get("1.0", "end-1c")
        try:
            data = render_png(text, w, h, style, halign_var.get(),
                              valign_var.get(), ss_factor=1,
                              fit=fit_var.get())
            state["photo"] = tk.PhotoImage(data=data)
            preview.configure(image=state["photo"], text="")
        except Exception as e:
            preview.configure(image="", text=f"Preview unavailable: {e}")

    def schedule_preview(*_):
        if state["job"]:
            root.after_cancel(state["job"])
        state["job"] = root.after(250, refresh_preview)

    text_box.bind("<KeyRelease>", schedule_preview)
    fit_var.trace_add("write", schedule_preview)
    halign_var.trace_add("write", schedule_preview)
    valign_var.trace_add("write", schedule_preview)
    width_var.trace_add("write", schedule_preview)
    height_var.trace_add("write", schedule_preview)
    root.bind("<Configure>", schedule_preview)
    def _selftest():
        # Drive the real widgets: change the background through its entry
        # variable exactly as the colour picker would, then inspect the
        # rendered preview bytes pixel (0,0).
        color_vars["background"].set("#123456")
        assert style.background == "#123456", style.background
        color_vars["background"].set("navy")
        assert style.background == "#000080", style.background
        root.update_idletasks()
        data = render_png(text_box.get("1.0", "end-1c"), 120, 90, style,
                          halign_var.get(), valign_var.get(), ss_factor=1,
                          fit=fit_var.get())
        # decode first pixel of our own PNG: unfilter is overkill — render
        # RGBA directly instead for the assertion.
        w, h, rgba = render_rgba(text_box.get("1.0", "end-1c"), 120, 90,
                                 style, halign_var.get(), valign_var.get(),
                                 ss_factor=1, fit=fit_var.get())
        px = tuple(rgba[0:3])
        print("SELFTEST style.background =", style.background,
              "| pixel(0,0) =", px, "| expected navy (0, 0, 128)")
        print("SELFTEST", "PASS" if px == (0, 0, 128) else "FAIL")
        root.destroy()

    if os.environ.get("SYSTERIAN_SELFTEST"):
        root.after(600, _selftest)
    root.after(400, refresh_preview)
    root.mainloop()
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1:
        sys.exit(cli(sys.argv[1:]))
    sys.exit(run_gui())
