// Spatial audio implementation.
#include "litholattice/audio/spatial_audio.h"
#include <algorithm>

namespace litholattice {

void SpatialAudio::emit(const SoundEvent& event) {
  events_.push_back(event);
}

void SpatialAudio::update(uint64_t currentTick, uint64_t maxAge) {
  events_.erase(
    std::remove_if(events_.begin(), events_.end(),
                   [currentTick, maxAge](const SoundEvent& e) {
                     // H-AUD-6: an event stamped with a FUTURE tick (e.g. the
                     // world reloaded and tick counters reset) is not old —
                     // the unsigned subtraction would wrap to a huge age and
                     // delete it instantly. Future events are kept.
                     return e.tick <= currentTick && currentTick - e.tick > maxAge;
                   }),
    events_.end());
}

}  // namespace litholattice
