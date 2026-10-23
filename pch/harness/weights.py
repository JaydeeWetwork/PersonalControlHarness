"""Where the PCH looks for model WEIGHTS (JD request, 2026-10-23).

Weights are the large model files you download yourself (for example a
Hugging Face folder with config.json and *.safetensors). They are NOT the
harness code: PCH_SPECIMEN_DIR points at the specimen's Python *code*
(default: the vendored third_party/DeepSeek-V3/inference). The PCH never
downloads weights.

Precedence (first one set wins):
  1. --weights <path>                 (command line)
  2. PCH_WEIGHTS_DIR                  (environment variable)
  3. "weights_dir" in a config file   (PCH_CONFIG, else ./pch.config.json,
                                       else the per-user config file)
  4. the default folder <repo>/models/weights   (gitignored)

Paths may contain spaces and may use Windows (C:\\Models\\My Model) or POSIX
(/home/me/my model) form; "~" and environment variables are expanded, and a
relative path in a config file is relative to that file's folder.
"""
import json
import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_DIR = os.path.join(REPO, "models", "weights")
ENV_VAR = "PCH_WEIGHTS_DIR"
CONFIG_ENV = "PCH_CONFIG"
CONFIG_NAME = "pch.config.json"
EXPECTED = ("config.json", "one or more *.safetensors files (or model.safetensors.index.json "
            "with every shard it lists)")


class WeightsError(Exception):
    """A weights location is missing or does not look like a weights folder."""


def user_config_path():
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, "pch", CONFIG_NAME)
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "pch", CONFIG_NAME)


def _norm(p, base=None):
    p = os.path.expandvars(os.path.expanduser(str(p).strip().strip('"')))
    if base and not os.path.isabs(p):
        p = os.path.join(base, p)
    return os.path.abspath(p)


def _config_candidates(env):
    if env.get(CONFIG_ENV):
        return [(_norm(env[CONFIG_ENV]), True)]     # explicit: must exist
    return [(os.path.abspath(CONFIG_NAME), False), (user_config_path(), False)]


def _from_config(env):
    for path, required in _config_candidates(env):
        if not os.path.isfile(path):
            if required:
                raise WeightsError(f"{CONFIG_ENV} points at {path!r}, which is not a file")
            continue
        try:
            with open(path, encoding="utf-8") as f:
                cfg = json.load(f)
        except (OSError, ValueError) as e:
            raise WeightsError(f"config file {path!r} is not valid JSON: {e}") from None
        if not isinstance(cfg, dict):
            raise WeightsError(f"config file {path!r} must hold a JSON object")
        if "weights_dir" in cfg:
            v = cfg["weights_dir"]
            if not isinstance(v, str) or not v.strip():
                raise WeightsError(f"config file {path!r}: weights_dir must be a non-empty string")
            return _norm(v, os.path.dirname(path)), f"config file {path}"
    return None


def resolve(cli=None, env=None):
    """Return (path, source) without checking the folder's contents."""
    env = os.environ if env is None else env
    if cli is not None and str(cli).strip():
        return _norm(cli), "--weights"
    if env.get(ENV_VAR, "").strip():
        return _norm(env[ENV_VAR]), ENV_VAR
    found = _from_config(env)
    if found:
        return found
    return DEFAULT_DIR, "default folder"


def validate(path, source="the given path"):
    """Raise WeightsError with what was expected unless `path` is a weights folder.
    Returns the list of weight files."""
    hint = f"expected a folder containing {EXPECTED[0]} and {EXPECTED[1]}"
    if not os.path.exists(path):
        raise WeightsError(f"weights folder not found: {path!r} (from {source}); {hint}. "
                           "The PCH does not download weights; download them yourself and point "
                           f"--weights or {ENV_VAR} at the folder.")
    if not os.path.isdir(path):
        raise WeightsError(f"weights path is a file, not a folder: {path!r} (from {source}); {hint}")
    names = set(os.listdir(path))
    missing = []
    if "config.json" not in names:
        missing.append("config.json")
    index = "model.safetensors.index.json"
    if index in names:
        try:
            with open(os.path.join(path, index), encoding="utf-8") as f:
                shards = sorted(set(json.load(f)["weight_map"].values()))
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
            raise WeightsError(f"{index} in {path!r} is unreadable: {e}") from None
        bad = [s for s in shards if os.path.basename(s) != s]
        if bad:
            raise WeightsError(f"{index} in {path!r} names shards outside the folder: {bad[:3]}")
        missing += [s for s in shards if s not in names]
        files = shards
    else:
        files = sorted(n for n in names if n.endswith(".safetensors"))
        if not files:
            missing.append("*.safetensors")
    if missing:
        raise WeightsError(f"weights folder {path!r} (from {source}) is incomplete: missing "
                           f"{', '.join(missing)}; {hint}")
    return files


def find(cli=None, env=None):
    """resolve() + validate(). Returns (path, source, files)."""
    path, source = resolve(cli, env)
    return path, source, validate(path, source)


if __name__ == "__main__":
    sys.exit("use pch/tools/pch_weights.py")
