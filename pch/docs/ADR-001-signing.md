# ADR-001: Receipt signing (Ed25519)

Status: Accepted (scheme chosen by JaydeeWetwork, 2026-10-23). Proposed by Cortana.
Task: S1.1 (DeepSeek PCH hardening plan, section 1).

## Context

PCH receipts (step-streams) are hash-chained: `chain = sha256(prev || canonical(record))`.
A hash chain detects accidental or partial edits, but anyone can re-hash a
forged log and pass the chain check (test bed 02, case 3). Until 0.0.0.4 the
trailer was also outside the chain. A receipt needs an authenticity proof that
an offline verifier can check with nothing but a public key.

## Decision

- **Scheme:** Ed25519 (RFC 8032), pure EdDSA, no pre-hash.
- **Library:** the Python `cryptography` package, which uses OpenSSL 3+
  (its wheels bundle OpenSSL; 50.0.2 bundles OpenSSL 4.0.3 on the dev
  workstation). It is installed by `pip install -r requirements.txt`.
  No hand-written curve code.
- **What is signed:** one signature per receipt, made once when the stream is
  closed (not per step). The signed bytes are a byte-exact, length-prefixed
  payload (below) over the trailer's own chain hash, the canonical trailer
  bytes, the trailer schema version, the brain ID and a timestamp.
- **Where it lives:** a sidecar file `<receipt>.sig` (JSON). The receipt file
  format is unchanged apart from the new chained trailer, so old readers and
  the shipped streams keep working. A sidecar also lets one receipt carry
  signatures from more than one key later without touching the chain.
- **Trailer in chain (S1.3):** trailer schema 2 adds `"schema": 2` and its
  own `chain` link computed like every other record. Editing any trailer
  field breaks the chain. Legacy trailers (no `schema`) are still readable
  but are labeled `legacy-unchained` and can never be signed or pass
  `pch-verify`.

## Signed payload (S1.2), byte-exact

All integers are little-endian. `lp(x)` = 4-byte unsigned length then the bytes.

| # | Field | Encoding |
|---|---|---|
| 1 | magic | 16 bytes: `PCH-RECEIPT-SIG\x00` |
| 2 | payload version | u16 = 1 |
| 3 | chain head | `lp(32 raw bytes)`: the trailer's own `chain` (covers every record and the trailer) |
| 4 | trailer bytes | `lp(canonical(trailer))`: sorted-key compact JSON without `chain`, ASCII |
| 5 | trailer schema | u32 (must be 2) |
| 6 | brain ID | `lp(UTF-8)`: derived from the header, never free text (see below) |
| 7 | timestamp | u64, Unix seconds (UTC) when the stream was closed |

Brain ID: `<brain.id>@<brain.version>` when the header has a `brain`
stamp, otherwise `weights:<first 16 hex of header.weights>`, otherwise
`unknown`. The verifier recomputes it from the header, so a sidecar cannot
claim a different brain.

Golden vectors: `pch/testdata/sig_vectors.json` (RFC 8032 test key 1,
public test-vector key, clearly labeled; never use it for real receipts).

## Parsing rules (canonicalization hardening)

Receipts and sidecars are parsed strictly: duplicate JSON keys, `NaN` /
`Infinity`, non-object lines and wrong types are rejected before any hashing.
The sidecar must have exactly the fields
`format, alg, key_id, brain_id, timestamp, chain_head, sig`.

## Consequences

- `pch-verify <log> <pubkey>` works offline on any machine with Python and
  `cryptography`; it never runs the harness or the model.
- Re-hash forgery (bed 02 case 3) is now caught: the forger can recompute
  the chain but not the signature.
- Key handling becomes a real responsibility: see `KEY-MANAGEMENT.md`.
- Signing cost is one Ed25519 signature per receipt (microseconds); measured
  numbers are printed by test bed 15.

## Alternatives considered

- libsodium via CMake: equally good primitive, but the PCH core is Python
  and libsodium has no first-party CMake build; it would add a C toolchain to
  the test-beds-only path.
- HMAC: needs a shared secret, so it is not offline-verifiable by strangers.
- Signing every step: no extra security over signing the chained trailer,
  more overhead.
