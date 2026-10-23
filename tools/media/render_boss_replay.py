"""Render the PCH HUD boss fight (bed 08 reference stub) to a 60 fps video,
frame-perfect: one frame per 60 Hz sim tick, drawn offline from the sealed
receipt, so the output does not depend on machine load.

  python tools/media/render_boss_replay.py OUT.mp4 [--scenario full_energy]
         [--seed 7] [--ticks 600] [--ffmpeg ffmpeg]

Needs Pillow and ffmpeg. Directed by JaydeeWetwork. Built by Cortana."""
import argparse
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "pch", "harness"))
import boss  # noqa: E402
import stepstream  # noqa: E402

W, H = 960, 540
BG, FG, DIM = (18, 20, 26), (226, 230, 238), (120, 128, 142)
BOSS_C, PLAYER_C, ENERGY_C, HP_C = (214, 80, 80), (90, 170, 240), (240, 196, 70), (110, 210, 120)


def font(size):
    for p in ("DejaVuSansMono.ttf", "consola.ttf", "C:/Windows/Fonts/consola.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "LiberationMono-Regular.ttf"):
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default(size)


def bar(d, x, y, w, val, cap, color, label, f):
    d.text((x, y - 22), label, fill=DIM, font=f)
    d.rectangle((x, y, x + w, y + 14), outline=DIM)
    if val is not None:
        d.rectangle((x + 1, y + 1, x + 1 + int((w - 2) * val / cap), y + 13), fill=color)


def frames(recs):
    f, fs, fb = font(18), font(14), font(26)
    steps = [r for r in recs if r.get("kind") == "step"]
    pos = float(steps[0]["hud"]["dist"])
    flash, wind_start, last = 0, None, None
    for r in steps:
        h = r["hud"]
        pos += (h["dist"] - pos) * 0.18          # deterministic easing per tick
        if h["windup"] and not last:
            wind_start = r["step"]
        last = h["windup"]
        if r.get("hit"):
            flash = 10
        img = Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(img)
        d.text((24, 18), "PCH boss bed 08: HUD-only brain, reference stub", fill=FG, font=f)
        d.text((24, 44), f"scenario {recs[0]['scenario']}  seed {recs[0]['run_seed']}  "
               f"tick {r['step']:4d} @ {recs[0]['tick_hz']} Hz", fill=DIM, font=fs)
        ground = 360
        d.line((60, ground, W - 60, ground), fill=DIM, width=2)
        bx = W - 220
        px = bx - 70 * pos
        sh = 6 if flash and flash % 2 else 0
        d.rectangle((bx - 40 + sh, ground - 150, bx + 40 + sh, ground), fill=BOSS_C)
        d.rectangle((px - 18, ground - 70, px + 18, ground), fill=PLAYER_C)
        if h["windup"]:
            k = min(1.0, (r["step"] - wind_start) / boss.WINDUP[h["windup"]])
            d.arc((bx - 70, ground - 220, bx + 70, ground - 80), 180, 180 + int(360 * k),
                  fill=ENERGY_C, width=6)
            d.text((bx - 60, ground - 250), f"wind-up: {h['windup']}", fill=ENERGY_C, font=fs)
        bar(d, 60, 110, 300, h["player_hp_bar"], 10, HP_C, "player HP", fs)
        bar(d, W - 360, 110, 300, h["boss_hp_bar"], 10, BOSS_C, "boss HP", fs)
        bar(d, W - 360, 165, 300, h["boss_energy_bar"], 10, ENERGY_C, "boss energy (HUD)", fs)
        d.text((60, 160), f"potions {h['potions']}", fill=FG, font=fs)
        ctrl = r.get("control") or "-"
        d.text((60, ground + 40), f"brain -> {r['action']:<8} control {ctrl}", fill=FG, font=fb)
        if r.get("hit"):
            d.text((60, ground + 80), f"hit: {r['hit']}", fill=BOSS_C, font=f)
        d.text((24, H - 30), f"receipt chain {r['chain'][:32]}...", fill=DIM, font=fs)
        flash = max(0, flash - 1)
        yield img.tobytes()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", help="output .mp4")
    ap.add_argument("--scenario", default="full_energy", choices=sorted(boss.SCENARIOS))
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--ticks", type=int, default=600)
    ap.add_argument("--ffmpeg", default="ffmpeg")
    a = ap.parse_args(argv)
    recs, _ = boss.run_hud_fight(a.scenario, a.seed, ticks=a.ticks)
    ok, why, _ = stepstream.check_sealed(recs)
    if not ok:
        print(f"ERROR: receipt not sealed: {why}", file=sys.stderr)
        return 1
    return encode(frames(recs), a.out, a.ffmpeg)


def encode(frame_iter, out, ffmpeg="ffmpeg", w=W, h=H):
    cmd = [ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
           "-s", f"{w}x{h}", "-r", "60", "-i", "-", "-c:v", "libx264", "-preset", "slow",
           "-crf", "26", "-pix_fmt", "yuv420p", "-r", "60", "-movflags", "+faststart", out]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    n = 0
    for fr in frame_iter:
        p.stdin.write(fr)
        n += 1
    p.stdin.close()
    rc = p.wait()
    print(f"{out}: {n} frames at 60 fps, ffmpeg rc={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
