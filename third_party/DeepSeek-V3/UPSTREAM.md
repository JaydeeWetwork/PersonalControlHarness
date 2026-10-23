# Vendored: DeepSeek-V3 reference inference code (code only)

- Upstream: https://github.com/deepseek-ai/DeepSeek-V3
- Pinned commit: `9b4e9788e4a3a731f7567338ed15d3ec549ce03b` (2025-08-28, "Merge pull request #969 from youkaichao/rmsnorm")
- Copied: `inference/` (Python code, configs, requirements) and `LICENSE-CODE`, byte for byte, unmodified.
- Not copied: model weights (none are used or redistributed), `LICENSE-MODEL`
  (applies only to the weights), figures, `README_WEIGHTS.md`.
- License: MIT, Copyright (c) 2023 DeepSeek (`LICENSE-CODE` in this folder).
- Authors: the DeepSeek team (DeepSeek-AI) and the DeepSeek-V3 designers. All credit
  for this code is theirs. The PCH imports it and never modifies it.
- Why vendored: JaydeeWetwork wants one clone to contain everything except
  model weights (2026-10-23).

To update: copy the same paths from a new upstream commit, update the commit
and the manifest below, and run `python testbeds/tb_vendored_specimen.py`.

## Manifest (sha256)

```
1c8f573e830ca9b3ebfeb7ace1823146e22b66f99ee223840e7637c9e745e1c7  LICENSE-CODE
610c665cf8e1fb0cdf9f2ab68833c1490cef7b1ef8cc8476a035b8ae38522179  inference/configs/config_16B.json
49db711e7c0b96dadd837a8830ae3cc0b9ad456087f0de313f955ec49539dc69  inference/configs/config_236B.json
d21d797509598ffaa7c22686abdf3a94e38f31a6193dc158fffe0a6fb6ef5010  inference/configs/config_671B.json
4baf1b049a10bc97f21b1388746c920478cc39590e18e5a66c26a6c19bfc1b4e  inference/configs/config_v3.1.json
a84c9972313dd768062613ba26e941b1e0d3e3b18c3b3e9a9ef78f55e7ea472b  inference/convert.py
7269820b32dedc2daddfdfabf1a974f0b5d5c76c1efd41bd2b169cb6b79d6b80  inference/fp8_cast_bf16.py
d1a5db4b9bc602ce5b7f875475c07e675c9c3aa58e5539f6cd187d0e1b813dec  inference/generate.py
ffee39fb93658b6ddd783c0932b1445cc4a007a061c0de2a40ac2a3b1563cc00  inference/kernel.py
fdb7995bc234badbf0011b5af8cf391be94343abc51305428cb9a04452116f02  inference/model.py
e5ba1286ec78fb919fbbca62d24757b289aea58b7499faf8b8d7c6e1fd4ea27c  inference/requirements.txt
```
