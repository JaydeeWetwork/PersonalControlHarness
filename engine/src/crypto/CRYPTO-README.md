# LithoLattice crypto module

Clean-room implementations (no third-party code):
- `sha256.cpp` / `sha512.cpp` — FIPS 180-4, verified against NIST test vectors.
- `ed25519.cpp` — RFC 8032 Ed25519, verified against the RFC 8032 test vectors
  and differential-tested against libsodium (via PyNaCl) during development.

NOT constant-time; not for high-threat contexts. The private signing key lives
offline with JD; signing happens on his machine. Verification uses public inputs.

Mod signing (the crude in-house trust layer, phase 1 of the signing roadmap):
- `signing.h` documents the canonical mod-hash spec byte-exactly.
- `litho_sign` CLI: keygen / sign / verify.
