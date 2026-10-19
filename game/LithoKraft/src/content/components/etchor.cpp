// EtchOR behaviour: the seam for the etch flow (wiring pass 2026-10-13,
// Team B item B6).
//
// Design (Glossary v2, IN DISCUSSION): the EtchOR is the single block that
// performs the etch -- powered by the Laysor, fed by the SelectORs. Its
// ONLY UI element is a name field: the SelectORs tell it WHAT to make, the
// name field tells it WHAT TO CALL IT -- the typed name becomes the output
// chip's name ("wafer in, ChipOR out, bearing the name-field value"; the
// name overwrites the chip's def name).
//
// Why a plain describe-only BlockBehavior, mirroring the certificate
// table's seam: the etch itself is an inventory operation (consume a
// wafer blank, produce a named ChipOR stack), and inventory lives on the
// game side, not in the signal sim. The wired flow therefore lives in
// game/etching.cpp (etchChip) with the `etch` console command as its
// entry point until JD designs the name-field UI; this class anchors the
// block's name and readout so the seam is a real class, not a dangling
// string. The SelectOR/ConnectOR/Laysor machine around the block is
// future work; nothing here fakes it.
#include "components.h"

namespace lithokraft::components {
namespace {

class EtchORBehavior final : public BlockBehavior {
 public:
  std::string describe(const BlockDef& d, CellState /*s*/) const override { return std::string(d.name); }
};

}  // namespace

void registerEtchOR(const Registrar& add) {
  add("etchor", std::make_unique<EtchORBehavior>());
}

}  // namespace lithokraft::components
