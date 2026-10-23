"""Weights location: precedence (CLI > PCH_WEIGHTS_DIR > config > default),
paths with spaces, Windows/POSIX forms, and clear errors for bad paths.
Uses empty placeholder files in a temp dir (no real weights, nothing
downloaded). With --local it instead validates the real configured weights folder
and SKIPS (exit 77) when none is present. Built by: Cortana."""
import json
import os
import subprocess
import sys
import tempfile

import pch_testlib
import weights as W

c = pch_testlib.Checks()
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pch", "tools", "pch_weights.py")


def fake(d, files=("config.json", "model.safetensors")):
    os.makedirs(d, exist_ok=True)
    for f in files:
        open(os.path.join(d, f), "w").close()
    return d


if "--local" in sys.argv:
    try:
        path, src, files = W.find()
    except W.WeightsError as e:
        pch_testlib.skip(f"no local weights folder to validate: {e}")
    c(f"real weights folder from {src} validates ({len(files)} files)", True, path)
    c.finish("LOCAL WEIGHTS")

with tempfile.TemporaryDirectory(prefix="pch weights ") as tmp:   # note the space
    cli = fake(os.path.join(tmp, "from cli"))
    envd = fake(os.path.join(tmp, "from env"))
    cfgd = fake(os.path.join(tmp, "from config"))
    cfg = os.path.join(tmp, "my config", "pch.config.json")
    os.makedirs(os.path.dirname(cfg))
    with open(cfg, "w") as f:
        json.dump({"weights_dir": "../from config"}, f)          # relative to the config file
    E = {"PCH_WEIGHTS_DIR": envd, "PCH_CONFIG": cfg}

    c("CLI beats env and config", W.resolve(cli, E) == (os.path.abspath(cli), "--weights"))
    c("env beats config", W.resolve(None, E) == (os.path.abspath(envd), "PCH_WEIGHTS_DIR"))
    p, src = W.resolve(None, {"PCH_CONFIG": cfg})
    c("config used when no CLI/env; relative path resolved from the config's folder",
      p == os.path.abspath(cfgd) and src.startswith("config file"), p)
    old = os.getcwd()
    os.chdir(tmp)                       # no ./pch.config.json here
    try:
        env = {"XDG_CONFIG_HOME": os.path.join(tmp, "none"), "APPDATA": os.path.join(tmp, "none")}
        c("default folder when nothing is set", W.resolve(None, env) == (W.DEFAULT_DIR, "default folder"))
    finally:
        os.chdir(old)
    c("blank CLI / blank env fall through", W.resolve("  ", {"PCH_WEIGHTS_DIR": " ", "PCH_CONFIG": cfg})[1].startswith("config"))
    c("quoted path with spaces is accepted", W.resolve(f'"{cli}"', {})[0] == os.path.abspath(cli))
    c("~ is expanded", W.resolve("~/x", {})[0] == os.path.abspath(os.path.expanduser("~/x")))
    c("folder with spaces validates", W.validate(cli) == ["model.safetensors"])
    if os.name == "nt":
        c("Windows drive path normalized", W.resolve("C:/Models/My Model", {})[0] == "C:\\Models\\My Model")
    else:
        c("POSIX path normalized", W.resolve("/opt/my models/./x/", {})[0] == "/opt/my models/x")

    def err(fn):
        try:
            fn()
        except W.WeightsError as e:
            return str(e)
        return ""

    m = err(lambda: W.validate(os.path.join(tmp, "nope"), "--weights"))
    c("missing folder: clear error naming the expected files", "not found" in m and "config.json" in m
      and "safetensors" in m and "does not download" in m, m[:90])
    m = err(lambda: W.validate(os.path.join(cli, "config.json")))
    c("a file instead of a folder is rejected", "is a file" in m, m[:60])
    m = err(lambda: W.validate(fake(os.path.join(tmp, "no cfg"), ("model.safetensors",))))
    c("missing config.json is reported", "missing config.json" in m, m[:80])
    m = err(lambda: W.validate(fake(os.path.join(tmp, "no st"), ("config.json",))))
    c("missing *.safetensors is reported", "*.safetensors" in m, m[:80])
    sh = fake(os.path.join(tmp, "sharded"), ("config.json", "a.safetensors"))
    with open(os.path.join(sh, "model.safetensors.index.json"), "w") as f:
        json.dump({"weight_map": {"x": "a.safetensors", "y": "b.safetensors"}}, f)
    m = err(lambda: W.validate(sh))
    c("sharded index with a missing shard is reported", "b.safetensors" in m, m[:90])
    with open(os.path.join(sh, "model.safetensors.index.json"), "w") as f:
        json.dump({"weight_map": {"x": "../../etc/passwd"}}, f)
    m = err(lambda: W.validate(sh))
    c("shard names escaping the folder are rejected", "outside the folder" in m, m[:80])
    bad = os.path.join(tmp, "bad.json")
    with open(bad, "w") as f:
        f.write("{not json")
    c("invalid config JSON: clear error", "not valid JSON" in err(lambda: W.resolve(None, {"PCH_CONFIG": bad})))
    c("PCH_CONFIG pointing nowhere: clear error", "not a file" in err(lambda: W.resolve(None, {"PCH_CONFIG": bad + "x"})))
    with open(bad, "w") as f:
        json.dump({"weights_dir": 5}, f)
    c("non-string weights_dir rejected", "non-empty string" in err(lambda: W.resolve(None, {"PCH_CONFIG": bad})))

    env = dict(os.environ, PCH_WEIGHTS_DIR=envd)
    r = subprocess.run([sys.executable, TOOL, "--weights", cli], capture_output=True, text=True, env=env)
    c("CLI tool: --weights wins and exits 0", r.returncode == 0 and "--weights" in r.stdout, r.stdout.strip()[-60:])
    r = subprocess.run([sys.executable, TOOL, "--weights", os.path.join(tmp, "nope")], capture_output=True, text=True, env=env)
    c("CLI tool: bad path exits 1 with the reason on stderr", r.returncode == 1 and "expected a folder" in r.stderr)
    r = subprocess.run([sys.executable, TOOL, "--bogus"], capture_output=True, text=True)
    c("CLI tool: usage error exits 2", r.returncode == 2)

c.finish("WEIGHTS LOCATION")
