"""Render a real ctest log as a smooth 60 fps terminal replay: lines appear at a
fixed rate and the view scrolls by a fixed number of pixels per frame (fixed
timestep, offline, frame-perfect). Absolute paths are stripped so nothing
personal is shown.

  python tools/media/render_terminal_replay.py CTEST_LOG OUT.mp4 [--lines-per-sec 14]

Directed by JaydeeWetwork. Built by Cortana."""
import argparse
import re
import sys

from PIL import Image, ImageDraw

from render_boss_replay import encode, font

W, H, LH, PAD = 960, 540, 22, 18
BG, FG, OK, BAD, DIM = (12, 14, 18), (214, 218, 226), (110, 210, 120), (230, 90, 90), (120, 128, 142)


def clean(line):
    line = re.sub(r"([A-Za-z]:)?[\\/][^\s]*[\\/]", "", line)   # drop directory parts of paths
    return line.rstrip()[:84]


def frames(lines, lps, hold):
    f = font(16)
    rows = (H - 2 * PAD) // LH
    shown_f = 60.0 / lps                  # frames per new line
    total = int(len(lines) * shown_f) + hold
    for i in range(total):
        n = min(len(lines), int(i / shown_f) + 1)
        frac = min(1.0, (i / shown_f) % 1.0) if n < len(lines) else 1.0
        scroll = max(0.0, (n - rows) + frac) if n >= rows else 0.0
        img = Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(img)
        first = int(scroll)
        off = (scroll - first) * LH
        for k, ln in enumerate(lines[first:n]):
            y = PAD + k * LH - off
            col = OK if "Passed" in ln or "tests passed" in ln else BAD if "Fail" in ln else \
                DIM if "Skipped" in ln else FG
            d.text((PAD, y), ln, fill=col, font=f)
        yield img.tobytes()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", help="ctest output text")
    ap.add_argument("out", help="output .mp4")
    ap.add_argument("--lines-per-sec", type=float, default=14.0)
    ap.add_argument("--max-lines", type=int, default=150)
    ap.add_argument("--ffmpeg", default="ffmpeg")
    a = ap.parse_args(argv)
    raw = open(a.log, encoding="utf-8", errors="replace").read().splitlines()
    lines = ["$ ctest --preset vs2026-release -j 8"] + [clean(x) for x in raw
             if re.search(r"^\s*\d+/\d+ Test +#|tests passed|Total Test", x)]
    lines = lines[:a.max_lines - 3] + [l for l in lines if "tests passed" in l or "Total Test" in l][-2:]
    return encode(frames(lines, a.lines_per_sec, hold=90), a.out, a.ffmpeg)


if __name__ == "__main__":
    sys.exit(main())
