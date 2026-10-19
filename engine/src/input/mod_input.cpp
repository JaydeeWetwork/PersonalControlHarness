// Mod input confinement (mod hardening item 6, 2026-10-11).
#include "litholattice/input/mod_input.h"

namespace litholattice {

bool ModInputRouter::isValidId(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

bool ModInputRouter::registerAction(const std::string& modId, const std::string& actionId,
                                    const std::string& displayName, std::string& err) {
  if (!isValidId(modId)) {
    err = "mod input: invalid mod id (want [a-z0-9_]{1,64})";
    return false;
  }
  if (!isValidId(actionId)) {
    err = "mod input: invalid action id (want [a-z0-9_]{1,64})";
    return false;
  }
  if (displayName.empty() || displayName.size() > 128) {
    err = "mod input: display name must be 1..128 characters";
    return false;
  }
  // Namespaced: a mod can never shadow an engine action or another mod's.
  const std::string namespaced = modId + ":" + actionId;
  if (actions_.find(namespaced) != actions_.end()) {
    err = "mod input: duplicate action '" + namespaced + "'";
    return false;
  }
  actions_[namespaced] = ModAction{modId, namespaced, displayName};
  order_.push_back(namespaced);
  return true;
}

size_t ModInputRouter::unregisterMod(const std::string& modId) {
  size_t removed = 0;
  std::vector<std::string> keep;
  keep.reserve(order_.size());
  for (const std::string& id : order_) {
    const auto it = actions_.find(id);
    if (it != actions_.end() && it->second.modId == modId) {
      actions_.erase(it);
      ++removed;
    } else {
      keep.push_back(id);
    }
  }
  order_.swap(keep);
  return removed;
}

std::vector<ModInputRouter::ModAction> ModInputRouter::actions() const {
  std::vector<ModAction> out;
  out.reserve(order_.size());
  for (const std::string& id : order_) {
    const auto it = actions_.find(id);
    if (it != actions_.end()) out.push_back(it->second);
  }
  return out;
}

bool ModInputRouter::hasAction(const std::string& namespacedId) const {
  return actions_.find(namespacedId) != actions_.end();
}

}  // namespace litholattice
