# LithoKraft: the game (game module, app = main + selftest runner, game pack assets/, scripts/),
# built on the LithoLattice engine modules from the root. Dependencies point game -> engine only.
project(LithoKraft VERSION 0.1.0 LANGUAGES C CXX)
add_subdirectory(src/content)
add_subdirectory(src/game)
add_subdirectory(src/app)
add_subdirectory(src/headless)
