// Input contexts: which actions a game mode listens to. The game declares its contexts in
// priority order (index = context id, higher = consulted first). Each action goes to the highest
// active context that lists it, and an opaque context hides every lower context's actions (so
// typing or an inventory never leak into movement).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "litholattice/input/action.h"

namespace litholattice {

struct ContextRules {
  std::string name;
  bool opaque = false;         ///< hides lower contexts' actions
  bool wantsText = false;      ///< receives the text channel
  bool activeAtStart = false;  ///< pushed when the InputSystem is created
  std::vector<ActionId> actions;
};

}  // namespace litholattice
