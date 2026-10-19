#include "components.h"

namespace lithokraft {

const char* facingName(int facing) {
  static constexpr const char* kNames[4] = {"N", "E", "S", "W"};
  return kNames[facing & 3];
}

const BehaviorCatalog& BehaviorCatalog::get() {
  static const BehaviorCatalog catalog;
  return catalog;
}

BehaviorCatalog::BehaviorCatalog() {
  const components::Registrar add = [this](std::string_view name, std::unique_ptr<BlockBehavior> b) {
    this->add(name, std::move(b));
  };
  components::registerPassive(add);
  components::registerLogic(add);
  components::registerInputs(add);
  components::registerSound(add);
  components::registerCertificateTable(add);
  components::registerEtchOR(add);

  // Bind every block that names a behaviour in the block registry.
  const auto& reg = blockRegistry();
  behaviors_.assign(reg.size(), &plain_);
  components_.assign(reg.size(), nullptr);
  for (size_t id = 0; id < reg.size(); ++id) {
    const std::string_view name = reg.def(BlockId(id)).behavior;
    if (name.empty()) continue;
    if (const BlockBehavior* b = byName(name)) {
      behaviors_[id] = b;
      components_[id] = dynamic_cast<const ComponentBehavior*>(b);
    }
  }
}

void BehaviorCatalog::add(std::string_view name, std::unique_ptr<BlockBehavior> behavior) {
  named_.emplace_back(name, std::move(behavior));
}

const BlockBehavior& BehaviorCatalog::behavior(BlockId b) const {
  return b < behaviors_.size() ? *behaviors_[b] : plain_;
}

const BlockBehavior* BehaviorCatalog::byName(std::string_view name) const {
  for (const auto& [n, b] : named_)
    if (n == name) return b.get();
  return nullptr;
}

}  // namespace lithokraft
