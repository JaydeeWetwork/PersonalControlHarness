#include "litholattice/audio/voice_allocator.h"
#include <algorithm>

namespace litholattice {

size_t VoiceAllocator::acquire(uint64_t key) {
  for (size_t i = 0; i < slots_.size(); ++i)
    if (slots_[i].key == key) return i;
  auto it = std::find_if(slots_.begin(), slots_.end(), [](const Slot& s) { return !s.key; });
  if (it == slots_.end()) {
    // Steal: virtualization happens BEFORE stealing - the oldest
    // virtualized (inaudible) voice goes first, so a new source never
    // kills an audible note while a virtualized voice sits idle. Only
    // when nothing is virtualized do we steal the oldest overall.
    auto virt = slots_.end();
    for (auto jt = slots_.begin(); jt != slots_.end(); ++jt)
      if (jt->virtualized && (virt == slots_.end() || jt->started < virt->started)) virt = jt;
    it = virt != slots_.end()
             ? virt
             : std::min_element(slots_.begin(), slots_.end(),
                              [](const Slot& a, const Slot& b) { return a.started < b.started; });
  }
  *it = {key, ++clock_, false};
  return size_t(it - slots_.begin());
}

void VoiceAllocator::setVirtualized(size_t voice, bool v) {
  if (voice < slots_.size()) slots_[voice].virtualized = v;
}

bool VoiceAllocator::virtualized(size_t voice) const {
  return voice < slots_.size() && slots_[voice].virtualized;
}

std::optional<size_t> VoiceAllocator::release(uint64_t key) {
  for (size_t i = 0; i < slots_.size(); ++i)
    if (slots_[i].key == key) { slots_[i].key.reset(); return i; }
  return std::nullopt;
}

void VoiceAllocator::clear() {
  for (Slot& s : slots_) {
    s.key.reset();
    s.virtualized = false;
  }
}

}  // namespace litholattice
