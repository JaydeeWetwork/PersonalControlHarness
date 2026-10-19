// Readout-only behaviours for blocks driven directly by the signal propagation (signal.cpp).
#include <format>
#include "components.h"
#include "lithokraft/content/signal.h"

namespace lithokraft::components {
namespace {

class Wire final : public BlockBehavior {
 public:
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | strength {}", d.name, stValue(s));
  }
};

/// Torches and plain lamps: state 1 = lit.
class OnOff final : public BlockBehavior {
 public:
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | {}", d.name, (s & 1) ? "ON" : "off");
  }
};

class ColorLamp final : public BlockBehavior {
 public:
  std::string describe(const BlockDef& d, CellState s) const override {
    const int v = stValue(s);
    if (!v) return std::format("{} | tier {} | off", d.name, d.tier);
    return std::format("{} | tier {} (wrap) | strength {} -> colour {}", d.name, d.tier, v, colorLampIndex(v, d.tier));
  }
};

}  // namespace

void registerPassive(const Registrar& add) {
  add("wire", std::make_unique<Wire>());
  add("on_off", std::make_unique<OnOff>());
  add("color_lamp", std::make_unique<ColorLamp>());
}

}  // namespace lithokraft::components
