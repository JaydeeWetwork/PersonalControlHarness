// LithoKraft is a game built on the LithoLattice engine. Game code lives in namespace lithokraft and
// uses engine names (litholattice::World, litholattice::ui::Button, ...) unqualified through this
// one using-directive, so the game reads like it did before the split. Engine code never sees it:
// nothing under src/ includes a lithokraft header.
#pragma once

namespace litholattice {}

namespace lithokraft {
using namespace litholattice;
}  // namespace lithokraft
