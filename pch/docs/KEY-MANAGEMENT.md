# Key management for PCH receipt signing

Proposed by Cortana (task S1.6). Scheme: Ed25519 (ADR-001).

## Generation

    python pch/tools/pch_keygen.py --name <purpose>-<yyyy>

- Default folder `~/.pch/keys`, outside every repository. The tool refuses to
  write into a git working tree, never overwrites, and prints only the key ID
  (first 16 hex of sha256 of the raw public key) and paths.
- Set `PCH_KEY_PASSPHRASE` before running to encrypt the private key
  (PKCS#8, best available encryption). Recommended for any key that signs
  receipts other people will rely on.
- `NAME.pub` is safe to publish. `NAME.key` is not.

## Storage

- The private key file is restricted to the current user (`chmod 600` on
  Linux/macOS, `icacls /inheritance:r /grant:r <user>:F` on Windows).
- Never commit private keys. `.gitignore` blocks `*.key`, `*.pem`, `keys/`
  and `.pch/`. The only private key in the repository is the RFC 8032
  TEST 1 key inside `pch/testdata/sig_vectors.json`; it is a published test
  vector, labeled `NOT A REAL KEY`, and must never sign a real receipt.
- Tests generate throwaway keys in memory or in an OS temp folder and delete
  them when they finish.
- HSM / hardware keys: not supported yet. Ed25519 on a YubiKey or a TPM-backed
  store is the natural next step; the sidecar format does not change.

## Rotation

- Rotate yearly, and at once on any suspected exposure. Name keys by
  purpose and year (`boss-receipts-2026`).
- Rotation does not invalidate old receipts: keep old public keys published
  (with their key IDs and validity dates) so old receipts stay verifiable.
- Each `.sig` names its `key_id`, so verifiers can pick the right public key.

## Revocation

A revocation list is a JSON file:

```json
{"format": "pch-revocations/1",
 "revoked": [{"key_id": "622287b9ee3a7381", "revoked_at": "2026-10-23", "reason": "laptop lost"}]}
```

`pch_verify.py <receipt> <key.pub> --revoked revoked.json` rejects any
receipt signed by a listed key. Limits, stated openly:

- Revocation is all-or-nothing per key (no "valid before date X" yet), because
  the sidecar timestamp is chosen by the signer and cannot prove when a
  receipt was really signed.
- The list itself is not signed; distribute it the same way as the public
  keys (the repository, a release asset), and pin its sha256 if you rely on it.

## Verification

    python pch/tools/pch_verify.py receipt.jsonl key.pub [--sig receipt.jsonl.sig] [--revoked FILE]

Exit 0 valid, 1 rejected (reason printed), 2 usage error. Needs Python and
`cryptography` only: no harness run, no model, no network.

## Tested

`testbeds/tb_key_rotation.py` (ctest `pch.key_rotation`) runs this document
end to end through the real CLIs. It generates a key, rotates to a new one,
checks that old receipts still verify with the old key, revokes the old key,
and confirms that its receipts are then rejected, including a back-dated one.
It also covers passphrase-encrypted keys and the keygen refusals.
`tb_verify_cli.py` covers wrong, revoked, missing and garbled keys and sidecars.
