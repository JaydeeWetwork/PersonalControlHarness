#!/usr/bin/env python3
"""Generate the LithoKraft certificate-signing glyph set.

Cistercian numerals (13th c., vertical-stave revival form): 36 component glyphs
(digits 1-9 x places units/tens/hundreds/thousands). Each glyph = full stave +
one digit stroke, drawn per the historical construction:
  units: upper right | tens: upper left (mirror) | hundreds: lower right (flip)
  thousands: lower left (mirror+flip)

JD's dot rule: a small round dot at EVERY vertex of the stroke graph —
line junctions, crossings, corners, and free ends.

Outputs SVG (scalable master) + PNG (transparent) in two colorways.
"""
import math, os, zipfile

OUT = os.path.dirname(os.path.abspath(__file__))
SW = 6.0          # stroke width
DOT_R = 5.4       # rivet dot radius
W, H = 100, 140   # viewBox
STAVE = ((50.0, 10.0), (50.0, 130.0))
COLORWAYS = {"violet": ("#A970FF", "#FF6F00"),   # (line, dot)
             "white": ("#FFFFFF", "#FF6F00")}

# --- unit strokes (upper right), as segment lists ---------------------------
U = {
    1: [((50, 10), (80, 10))],
    2: [((50, 50), (80, 50))],
    3: [((50, 10), (80, 40))],
    4: [((50, 50), (80, 20))],
    5: [((50, 10), (80, 10)), ((80, 10), (50, 40))],
    6: [((78, 12), (78, 42))],                       # detached dash
    7: [((50, 10), (80, 10)), ((80, 10), (80, 40))],
    8: [((50, 50), (80, 50)), ((80, 50), (80, 80))],
    9: [((50, 10), (80, 10)), ((80, 10), (80, 40)), ((80, 40), (50, 40))],
}

def mx(seg):  # mirror left-right
    return ((100 - seg[0][0], seg[0][1]), (100 - seg[1][0], seg[1][1]))

def fy(seg):  # flip top-bottom
    return ((seg[0][0], 140 - seg[0][1]), (seg[1][0], 140 - seg[1][1]))

def glyph_segments(place, digit):
    segs = [tuple(map(tuple, s)) for s in U[digit]]
    if place in ("tens", "thousands"):
        segs = [mx(s) for s in segs]
    if place in ("hundreds", "thousands"):
        segs = [fy(s) for s in segs]
    return [STAVE] + segs

# --- vertex/dot computation --------------------------------------------------
EPS = 1e-9

def sub(p, q): return (p[0] - q[0], p[1] - q[1])
def dotp(a, b): return a[0] * b[0] + a[1] * b[1]
def crossp(a, b): return a[0] * b[1] - a[1] * b[0]

def seg_intersection(s1, s2):
    """Return intersection point of two segments, or None."""
    p, r = s1[0], sub(s1[1], s1[0])
    q, s = s2[0], sub(s2[1], s2[0])
    denom = crossp(r, s)
    qp = sub(q, p)
    if abs(denom) < EPS:  # parallel (incl. collinear)
        return None
    t = crossp(qp, s) / denom
    u = crossp(qp, r) / denom
    if -EPS <= t <= 1 + EPS and -EPS <= u <= 1 + EPS:
        return (p[0] + t * r[0], p[1] + t * r[1])
    return None

def vertices(segs):
    pts = []
    for s in segs:
        pts += [s[0], s[1]]
    n = len(segs)
    for i in range(n):
        for j in range(i + 1, n):
            p = seg_intersection(segs[i], segs[j])
            if p:
                pts.append(p)
    # dedupe
    uniq = []
    for p in pts:
        if not any(math.hypot(p[0] - q[0], p[1] - q[1]) < 0.5 for q in uniq):
            uniq.append(p)
    return uniq

# --- svg rendering -----------------------------------------------------------
def svg_doc(body, w=W, h=H):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}">'
            f'{body}</svg>')

# Halo technique: every stroke and rivet is drawn twice — first a slightly larger
# pass in the halo color (the "thin outline"), then the main pass on top.
# on_dark variant: white halo (for dark backgrounds); on_light: black halo.
HALO_W = 2.0
BG_HALO = {"on_dark": "#FF3B30", "on_light": "#2F7CF6"}

def _halo_svg(segs, line, dot, halo):
    d = ''
    for s in segs:
        d += f'M{s[0][0]} {s[0][1]}L{s[1][0]} {s[1][1]}'
    parts = [f'<path d="{d}" fill="none" stroke="{halo}" stroke-width="{SW + 2*HALO_W}" '
             f'stroke-linecap="round" stroke-linejoin="round"/>']
    for p in vertices(segs):
        parts.append(f'<circle cx="{p[0]:.2f}" cy="{p[1]:.2f}" r="{DOT_R + HALO_W}" fill="{halo}"/>')
    parts.append(f'<path d="{d}" fill="none" stroke="{line}" stroke-width="{SW}" '
                 f'stroke-linecap="round" stroke-linejoin="round"/>')
    for p in vertices(segs):
        parts.append(f'<circle cx="{p[0]:.2f}" cy="{p[1]:.2f}" r="{DOT_R}" fill="{dot}"/>')
    return svg_doc(''.join(parts))

def glyph_svg(place, digit, line, dot, halo):
    return _halo_svg(glyph_segments(place, digit), line, dot, halo)

def composed_svg(number, line, dot, halo):
    """Compose a full 1..9999 numeral on one stave (dots deduped)."""
    n = number
    thou, n = divmod(n, 1000)
    hund, n = divmod(n, 100)
    ten, one = divmod(n, 10)
    segs = [STAVE]
    if one:   segs += glyph_segments("units", one)[1:]
    if ten:   segs += glyph_segments("tens", ten)[1:]
    if hund:  segs += glyph_segments("hundreds", hund)[1:]
    if thou:  segs += glyph_segments("thousands", thou)[1:]
    return _halo_svg(segs, line, dot, halo)

if __name__ == "__main__":
    import cairosvg

    PLACES = [("units", 1), ("tens", 10), ("hundreds", 100), ("thousands", 1000)]
    os.makedirs(OUT, exist_ok=True)
    made = []
    for cname, (color, dot) in COLORWAYS.items():
        for bname, halo in BG_HALO.items():
            for place, mult in PLACES:
                for digit in range(1, 10):
                    val = digit * mult
                    name = f"cistercian_{val}"
                    sdir, pdir = f"{OUT}/svg/{cname}/{bname}", f"{OUT}/png/{cname}/{bname}"
                    os.makedirs(sdir, exist_ok=True); os.makedirs(pdir, exist_ok=True)
                    svg = glyph_svg(place, digit, color, dot, halo)
                    sp = f"{sdir}/{name}.svg"
                    open(sp, "w").write(svg)
                    pp = f"{pdir}/{name}.png"
                    cairosvg.svg2png(bytestring=svg.encode(), write_to=pp,
                                     output_width=700, output_height=980,
                                     background_color="transparent")
                    made.append(pp)

    # composed examples (both background variants)
    os.makedirs(f"{OUT}/composed", exist_ok=True)
    for bname, halo in BG_HALO.items():
        os.makedirs(f"{OUT}/composed/{bname}", exist_ok=True)
        for num in (1111, 2026, 9999):
            svg = composed_svg(num, *COLORWAYS["violet"], halo)
            open(f"{OUT}/composed/{bname}/cistercian_{num}.svg", "w").write(svg)
            cairosvg.svg2png(bytestring=svg.encode(),
                             write_to=f"{OUT}/composed/{bname}/cistercian_{num}.png",
                             output_width=700, output_height=980,
                             background_color="transparent")

    # specimen sheets (one per background variant)
    cell_w, cell_h = 170, 220
    cols, rows = 9, 4
    for bname, halo in BG_HALO.items():
        bg = "#0a0a12" if bname == "on_dark" else "#f4f1ea"
        sheet = [f'<svg xmlns="http://www.w3.org/2000/svg" '
                 f'viewBox="0 0 {cols*cell_w} {rows*cell_h}">']
        for ri, (place, mult) in enumerate(PLACES):
            for digit in range(1, 10):
                x, y = (digit - 1) * cell_w, ri * cell_h
                inner = glyph_svg(place, digit, *COLORWAYS["violet"], halo)
                body = inner[inner.find(">") + 1:inner.rfind("</svg>")]
                sheet.append(f'<svg x="{x+35}" y="{y+10}" width="100" height="140" viewBox="0 0 100 140">{body}</svg>')
                val = digit * mult
                sheet.append(f'<text x="{x+cell_w/2}" y="{y+190}" text-anchor="middle" '
                             f'font-family="sans-serif" font-size="22" fill="#888">{val}</text>')
        sheet.append("</svg>")
        sheet_svg = "".join(sheet)
        open(f"{OUT}/specimen_{bname}.svg", "w").write(sheet_svg)
        cairosvg.svg2png(bytestring=sheet_svg.encode(), write_to=f"{OUT}/specimen_{bname}.png",
                         output_width=cols * cell_w, background_color=bg)
    # keep specimen.png as the on_dark sheet for continuity
    import shutil
    shutil.copy(f"{OUT}/specimen_on_dark.png", f"{OUT}/specimen.png")

    # zip everything
    zpath = f"{OUT}/cistercian-glyphs.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(OUT):
            for f in files:
                if f == "generate.py" or f.endswith(".zip"):
                    continue
                fp = os.path.join(root, f)
                z.write(fp, os.path.relpath(fp, OUT))
    print(f"done: {len(made)} glyph pngs + svgs, specimen, composed examples, zip")
