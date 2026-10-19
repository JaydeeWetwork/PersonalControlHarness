// Sound-making components.
#include <format>
#include "components.h"
#include "lithokraft/content/music.h"

namespace lithokraft::components {
namespace {

/// Back input strength = pitch (1..64 -> C2 upward in semitones), 0 = silent. Consumes the
/// signal (no output). Reports a Tone cue whenever the pitch changes; the game plays it on a
/// SID3 voice.
class ToneBlock final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory&) const override {
    const int f = stFacing(st), v = in.back(f);
    ComponentStep step{0, 0, makeState(v, f, 0, v > 0)};
    if (v != stValue(st)) step.sound = SoundCue::Tone;
    return step;
  }
  int emitToward(int, CellState, const ComponentMemory&) const override { return 0; }
  std::string describe(const BlockDef& d, CellState s) const override {
    const int v = stValue(s);
    if (!v) return std::format("{} | silent | facing {}", d.name, facingName(stFacing(s)));
    const int note = noteForStrength(v);
    return std::format("{} | s={} -> {} ({:.1f} Hz) | facing {}", d.name, v, noteName(note), noteFrequency(note),
                       facingName(stFacing(s)));
  }
};

}  // namespace

void registerSound(const Registrar& add) { add("tone", std::make_unique<ToneBlock>()); }

}  // namespace lithokraft::components
