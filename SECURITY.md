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

- Receipts are hashed, not signed. They detect accidental or partial
  tampering, not someone who recomputes the whole chain.
- The receipt trailer is not yet covered by the hash chain. The test-bed
  verifier checks it as a stopgap; a core fix is pending.

Reports that go beyond these known limitations are especially welcome.
