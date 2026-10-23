"""CPU bridge shim for the specimen's kernel module.

model.py does `from kernel import act_quant, weight_dequant, fp8_gemm`, and
the real kernel.py imports Triton (GPU-only). The harness runs the bf16
path exclusively, where none of these are ever called -- Linear.forward
branches to F.linear for bf16. This shim lets the specimen import cleanly
on CPU WITHOUT modifying the specimen, and fails loudly if an FP8 path is
ever reached by accident: honest failure over silent substitution.
"""


def _unavailable(name):
    def f(*args, **kwargs):
        raise RuntimeError(
            f"kernel.{name} reached on the CPU harness: FP8 kernel paths are "
            "out of scope for this prototype (bf16 path only)."
        )
    return f


act_quant = _unavailable("act_quant")
weight_dequant = _unavailable("weight_dequant")
fp8_gemm = _unavailable("fp8_gemm")
