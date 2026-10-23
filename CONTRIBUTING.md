# Contributing

Thanks for your interest. This repository holds the Personal Control Harness
(PCH), its test beds, and the LithoLattice engine and LithoKraft game that it
runs against. Issues and pull requests are welcome.

## Before you open a pull request

1. Build and run the tests as described in [BUILDING.md](BUILDING.md):

       cmake --preset vs2026            # or: cmake --preset testbeds-only
       cmake --build --preset vs2026-release
       ctest --preset vs2026-release

   A test that cannot run in your environment (for example the symlink tests
   without Windows Developer Mode, or model tests without PyTorch) reports
   **Skipped**, not Passed. Please say in the pull request which tests were
   skipped for you.
2. Keep changes small and focused, with one logical change per commit.
3. Do not weaken a test to make it pass. If a test is wrong, explain why in
   the commit message.
4. Do not commit machine-specific paths, credentials or key material.

## Changes to the PCH core

`pch/harness/` is the PCH core. Changes there (for example to the receipt
format or the hash chain) need the maintainer's approval before they are
merged. Test-bed changes under `testbeds/` use the harness read-only and do not
need that step. See the known issues in
[testbeds/SUMMARY.md](testbeds/SUMMARY.md) before proposing a format change.

## License

By contributing you agree that your contribution is licensed under the
repository's [MIT License](LICENSE).
