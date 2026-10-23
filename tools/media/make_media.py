"""Regenerate docs/media: three frame-perfect 60 fps H.264 MP4s plus looping
animated-WebP previews for the README. Everything is rendered offline from
deterministic sources (fixed seed, fixed 1/60 s timestep), never screen-captured,
so nothing personal can appear and the result does not depend on machine load.

  python tools/media/make_media.py --flyover-exe PATH/TO/flyover_capture \
         --ctest-log CTEST_OUTPUT.txt [--ffmpeg ffmpeg]

Directed by JaydeeWetwork. Built by Cortana."""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MEDIA = os.path.join(HERE, "..", "..", "docs", "media")
sys.path.insert(0, HERE)


def run(cmd):
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def preview(ff, mp4, webp, seconds, start=0.0, fps=60):
    # 400 px wide, lossy WebP, infinite loop. The flyover preview is 30 fps to
    # stay under ~3 MB (its MP4 is the full 60 fps); the others stay at 60 fps.
    run([ff, "-y", "-loglevel", "error", "-ss", str(start), "-t", str(seconds), "-i", mp4,
         "-vf", f"fps={fps},scale=400:-2:flags=lanczos", "-c:v", "libwebp", "-q:v", "35",
         "-compression_level", "6", "-loop", "0", "-an", webp])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--flyover-exe", required=True, help="built flyover_capture executable")
    ap.add_argument("--ctest-log", required=True, help="a full ctest run's output")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    a = ap.parse_args(argv)
    ff = a.ffmpeg
    os.makedirs(MEDIA, exist_ok=True)
    out = {k: os.path.join(MEDIA, k + ".mp4") for k in ("pch-boss-hud-replay", "pch-testbeds-terminal",
                                                       "lithokraft-flyover")}
    import render_boss_replay
    import render_terminal_replay
    if render_boss_replay.main([out["pch-boss-hud-replay"], "--ffmpeg", ff]):
        return 1
    if render_terminal_replay.main([a.ctest_log, out["pch-testbeds-terminal"], "--ffmpeg", ff]):
        return 1
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, "flyover.rgb")
        run([a.flyover_exe, raw, "480", "960", "540"])
        run([ff, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "960x540",
             "-r", "60", "-i", raw, "-c:v", "libx264", "-preset", "slow", "-crf", "27",
             "-pix_fmt", "yuv420p", "-movflags", "+faststart", out["lithokraft-flyover"]])
    preview(ff, out["pch-boss-hud-replay"], os.path.join(MEDIA, "pch-boss-hud-replay.webp"), 4)
    preview(ff, out["pch-testbeds-terminal"], os.path.join(MEDIA, "pch-testbeds-terminal.webp"), 4, 3)
    preview(ff, out["lithokraft-flyover"], os.path.join(MEDIA, "lithokraft-flyover.webp"), 8, fps=30)
    return 0


if __name__ == "__main__":
    sys.exit(main())
