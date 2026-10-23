# Security policy

## Reporting a vulnerability

Please do not open a public issue for a security problem. Report it privately
through GitHub: open the repository's **Security** tab and choose
**Report a vulnerability**. Include the steps to reproduce, the affected files,
and the commit you tested.

## Scope and known limitations

The PCH records each step in a hash-chained receipt. Please keep these
documented limitations in mind (details in
[testbeds/SUMMARY.md](testbeds/SUMMARY.md)):

- Receipts are signed with Ed25519 (since 0.0.0.4-alpha; ADR-001) and the
  trailer is in the hash chain. A signature proves which key signed a
  receipt, not that the signer was honest: protect signing keys
  ([pch/docs/KEY-MANAGEMENT.md](pch/docs/KEY-MANAGEMENT.md)).
- Shipped reference streams created before 0.0.0.4 have unchained trailers
  and no signatures; the core labels them `legacy-unchained` and
  `pch-verify` refuses them.
- Revocation lists are not signed and revocation is per key, not per date.

Reports that go beyond these known limitations are especially welcome.
