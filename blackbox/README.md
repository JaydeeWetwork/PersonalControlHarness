# Black Box prototype (standalone C11)

"Black Box Mystery Solved": a tamper-evident, replayable decision log. Design:
[docs/blackbox/BLACKBOX_DESIGN.md](../docs/blackbox/BLACKBOX_DESIGN.md).
**Prototype only; not wired into the engine.** In PCH it runs as test bed 16
([testbeds/examples/16-blackbox-replay-tamper.md](../testbeds/examples/16-blackbox-replay-tamper.md)).

Built by the top-level CMake (Windows MSVC and Linux GCC/Clang), targets
`bbtest`, `bbverify`, `bbbench`:

    ctest --preset vs2026-release -R blackbox     # Windows
    ctest --test-dir build -R blackbox            # Linux

Standalone on Linux/macOS: `make`, `make test`, `make asan`, `make tsan`.
`bb_platform.h` holds the only OS calls (thread, sleep, fsync, clock).

**The checkpoint signer here is a TEST-ONLY HMAC-SHA256 with a fixed, public
test key.** It is not a real signature, must never be used outside tests, and
is unrelated to the PCH receipt signing and the Release signing check. No real
keys are used or read.
