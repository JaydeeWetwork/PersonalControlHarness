"""Prototype control harness over the DeepSeek-V3 reference model code.

Implements the Pillar Plug-In Map (2026-10-18) at toy scale, on CPU:
  Pillar 1 -- per-step seeded sampling: the sampler's draw at step t derives
              from (run_seed, t) alone, so the stream is self-describing and
              replayable; committed tokens are the hashed state.
  Pillar 2 -- control enters only at step boundaries, serialized in-stream.
  Pillar 3 -- outbound facts (prefill sealed, eos) leave as step-stamped
              event records on the same chain.

Prefill is one sealed macro-step (chunk geometry recorded); decode commits
at every token boundary -- per JD's map addendum answering Argon's question.

The specimen (DeepSeek-V3/inference) is imported, never modified.
"""
import hashlib
import os
import platform as _platform
import sys

import torch

_HERE = os.path.dirname(os.path.abspath(__file__))
# Specimen location (release-package wiring): clone DeepSeek-V3 beside this
# repo so that ../DeepSeek-V3/inference resolves, or point PCH_SPECIMEN_DIR
# at an existing DeepSeek-V3/inference directory. The specimen is imported,
# never modified, and is NOT distributed with this package.
_SPECIMEN = os.environ.get(
    "PCH_SPECIMEN_DIR", os.path.join(_HERE, "..", "DeepSeek-V3", "inference")
)
sys.path.insert(0, os.path.abspath(_SPECIMEN))
sys.path.insert(0, _HERE)  # harness dir first: our kernel.py shim shadows the Triton one

import model as dsm  # noqa: E402  (DeepSeek-V3 reference implementation)
import stepstream  # noqa: E402


def toy_args() -> dsm.ModelArgs:
    """A DeepSeek-V3 architecture shrunk to CPU-toy scale. Same code paths:
    MLA attention, MoE routing, rotary embeddings -- just smaller numbers."""
    return dsm.ModelArgs(
        max_batch_size=1,
        max_seq_len=256,
        dtype="bf16",
        vocab_size=512,
        dim=128,
        inter_dim=256,
        moe_inter_dim=64,
        n_layers=3,
        n_dense_layers=1,
        n_heads=4,
        n_routed_experts=8,
        n_shared_experts=1,
        n_activated_experts=2,
        n_expert_groups=1,
        n_limited_groups=1,
        score_func="sigmoid",
        kv_lora_rank=64,
        qk_nope_head_dim=32,
        qk_rope_head_dim=16,
        v_head_dim=32,
        original_seq_len=256,
        rope_factor=1.0,
    )


def build_model(init_seed: int = 20261018) -> dsm.Transformer:
    torch.set_num_threads(1)  # one thread: reduction order pinned (Argon's hazard #1)
    torch.set_default_dtype(torch.bfloat16)  # as the specimen's own __main__ does
    torch.manual_seed(init_seed)
    model = dsm.Transformer(toy_args())
    # Fixture calibration: the specimen leaves its Linear weights as
    # uninitialized torch.empty, expecting a checkpoint load to fill them.
    # With no checkpoint at toy scale, WE fill them -- deterministically,
    # from a seeded generator -- so the "weights" the stream attests are a
    # real, reproducible fixture rather than allocator weather.
    g = torch.Generator().manual_seed(init_seed)
    with torch.no_grad():
        for name, p in model.named_parameters():
            if p.dim() >= 2:
                p.normal_(0.0, 0.05, generator=g)
            elif "norm" in name:
                p.fill_(1.0)
            else:
                p.zero_()
        # Same story for buffers the specimen expects a checkpoint to fill
        # (e.g. the MoE gate's correction bias): zero them deterministically.
        # freqs_cis is computed from args in __init__ -- never touched.
        for name, b in model.named_buffers():
            if "freqs_cis" not in name:
                b.zero_()
    model.eval()
    return model


def _raw(t: torch.Tensor) -> bytes:
    t = t.detach().cpu().contiguous()
    # Bound the storage read to the tensor's own bytes: a prefix view's
    # storage spans the whole buffer, and uncommitted tail state must never
    # leak into a seal. (Found by the Disc Test itself: a reused model's
    # dirty cache tail once poisoned a digest it had no business touching.)
    return bytes(t.untyped_storage()[: t.numel() * t.element_size()])


def weights_digest(model) -> str:
    """Digest over parameters only (runtime caches are state, not weights)."""
    h = hashlib.sha256()
    for name, p in model.named_parameters():
        h.update(name.encode())
        h.update(_raw(p))
    return h.hexdigest()


def cache_digest(model, upto: int) -> str:
    """Digest of the MLA latent caches over positions [0, upto) -- the
    snapshot state the Additional Renderer would rehearse from."""
    h = hashlib.sha256()
    for layer in model.layers:
        attn = layer.attn
        for buf in (attn.kv_cache, attn.pe_cache):
            h.update(_raw(buf[0, :upto]))
    return h.hexdigest()


def derive_step_seed(run_seed: int, step: int) -> int:
    d = hashlib.sha256(f"ll-harness:{run_seed}:{step}".encode()).digest()
    return int.from_bytes(d[:8], "little") % (2**63)


def sample_seeded(logits: torch.Tensor, temperature: float, run_seed: int, step: int) -> int:
    """The specimen's sampler (generate.py sample()) with the ambient RNG
    replaced by a draw derived from (run_seed, step) -- Pillar 1's one seam."""
    if temperature <= 0:
        return int(torch.argmax(logits, dim=-1).item())
    probs = torch.softmax(logits / temperature, dim=-1)
    g = torch.Generator()
    g.manual_seed(derive_step_seed(run_seed, step))
    noise = torch.empty_like(probs).exponential_(1.0, generator=g)
    return int(torch.argmax(probs / noise, dim=-1).item())


def chosen_prob(logits: torch.Tensor, temperature: float, token: int) -> float:
    """Live probability of a token under the current logits. Recorded in the
    step record so a WITNESSED verifier (different arithmetic, same math)
    can check the token it sees against the probability the live run saw --
    Argon's epsilon criterion needs the recorded draw, not just the token."""
    if temperature <= 0:
        return 1.0 if int(torch.argmax(logits, dim=-1).item()) == token else 0.0
    probs = torch.softmax(logits.float() / temperature, dim=-1)
    return float(probs[0, token].item())


def platform_info() -> dict:
    """Platform that recorded a stream (core fix approved by JD 2026-10-23).
    Float results (and so the prefill cache digest) can differ between OS /
    CPU arch / torch builds, so the header names them. The header is hashed
    into the chain, so this tag cannot be swapped without breaking it."""
    return {"os": _platform.system(), "arch": _platform.machine(),
            "torch": torch.__version__, "python": _platform.python_version()}


def platform_key(info: dict) -> tuple:
    """What a reference stream must match to be replayable bit-for-bit here:
    OS, CPU arch (amd64 == x86_64) and torch version. Python is recorded for
    the receipt but does not change torch's float results."""
    arch = str(info.get("arch", "")).lower().replace("amd64", "x86_64")
    return (str(info.get("os", "")).lower(), arch, str(info.get("torch", "")))


def run_live(model, prompt, max_new, temperature, run_seed, control_at=None):
    """Execute a run under the harness; returns the sealed step-stream."""
    records = [{
        "kind": "header", "run_seed": run_seed, "temperature": temperature,
        "weights": weights_digest(model), "prompt": list(prompt),
        "platform": platform_info(),
    }]
    tokens = torch.tensor([list(prompt)], dtype=torch.long)
    with torch.no_grad():
        logits = model.forward(tokens, 0)  # prefill: one macro-step
    records.append({
        "kind": "prefill", "chunk": [0, len(prompt)],
        "cache": cache_digest(model, len(prompt)),
    })
    prev_token = sample_seeded(logits, temperature, run_seed, 0)
    records.append({"kind": "step", "step": 0, "token": prev_token,
                    "p": chosen_prob(logits, temperature, prev_token)})
    produced = [prev_token]
    for t in range(1, max_new):
        if control_at is not None and t == control_at:
            # Control admitted ONLY here: at the boundary, in the stream.
            records.append({"kind": "control", "step": t, "op": "operator-note",
                            "payload": "checkpoint"})
        cur_pos = len(prompt) + t - 1
        with torch.no_grad():
            logits = model.forward(torch.tensor([[prev_token]]), cur_pos)
        prev_token = sample_seeded(logits, temperature, run_seed, t)
        records.append({"kind": "step", "step": t, "token": prev_token,
                        "p": chosen_prob(logits, temperature, prev_token)})
        produced.append(prev_token)
    stepstream.close(records)  # schema-2 trailer, itself in the chain (S1.3)
    return records, produced


def run_live_signed(model, prompt, max_new, temperature, run_seed, private_key,
                    control_at=None, timestamp=None):
    """run_live, then sign the closed receipt once (S1.4). Returns
    (records, produced, sidecar); save the sidecar next to the stream as
    <stream>.sig. The key is used only here and never written anywhere."""
    import receipt_sig  # noqa: E402  (needs the 'cryptography' package)
    records, produced = run_live(model, prompt, max_new, temperature, run_seed, control_at)
    return records, produced, receipt_sig.sign_receipt(records, private_key, timestamp)


def verify_replay(model, records):
    """Replay a stream against fresh model state. Returns (ok, reason).
    Teacher-forced: recorded tokens are fed back as inputs; every committed
    token must re-derive from its recorded per-step draw, the prefill cache
    digest must match, and the hash chain must recompute to the trailer."""
    ok, why, _label = stepstream.check_sealed(records)
    if not ok:
        return False, f"receipt check failed: {why}"
    header = records[0]
    if header.get("weights") != weights_digest(model):
        return False, "weights digest mismatch (wrong model state submitted)"
    prompt = header["prompt"]
    temperature, run_seed = header["temperature"], header["run_seed"]
    steps = [r for r in records if r["kind"] == "step"]
    tokens = torch.tensor([list(prompt)], dtype=torch.long)
    with torch.no_grad():
        logits = model.forward(tokens, 0)
    pre = next(r for r in records if r["kind"] == "prefill")
    if cache_digest(model, len(prompt)) != pre["cache"]:
        return False, "prefill cache digest mismatch"
    for rec in steps:
        t = rec["step"]
        if t > 0:
            cur_pos = len(prompt) + t - 1
            prev = steps[t - 1]["token"]
            with torch.no_grad():
                logits = model.forward(torch.tensor([[prev]]), cur_pos)
        got = sample_seeded(logits, temperature, run_seed, t)
        if got != rec["token"]:
            return False, f"step {t}: re-derived token {got} != recorded {rec['token']}"
    trailer = records[-1]
    sealed = [r for r in records if r["kind"] != "trailer"]
    if trailer["final_chain"] != sealed[-1]["chain"]:
        return False, "trailer seal mismatch"
    return True, f"all {len(steps)} committed tokens re-derived; chain + seal intact"
