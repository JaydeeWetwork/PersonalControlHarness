# Scratch-copy step for the headless harness (see src/headless/CMakeLists.txt).
# The tree's blocks.cpp is NEVER modified by this script.
#
# History (G-L3, review 2026-10-13): this script used to patch out a
# row-count guard in blocks.cpp that called std::abort() on a registry
# desync. That guard no longer exists — the resolveId registry refactor
# removed it entirely — so the old string replacement matched nothing and
# the "patch" was a no-op (its only effect was adding a duplicate
# #include <cstdio> that blocks.cpp already has). The harness now compiles
# a VERBATIM scratch copy of blocks.cpp: the copy exists so the harness
# binaries link the content registry from an object built in their own
# target (see CMakeLists.txt), not to change its contents. If a future
# change needs a real patch again, add it here with its reason.
file(READ "${SRC}" text)
file(WRITE "${DST}" "${text}")
