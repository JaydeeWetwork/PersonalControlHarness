#include "litholattice/net/priority.h"

#include <algorithm>

namespace litholattice {
namespace net {

std::vector<StreamGrant> PrioManager::allocate(uint32_t totalBytes) const {
  // Deterministic order: by streamId ascending throughout.
  std::vector<StreamDef> defs = streams_;
  std::sort(defs.begin(), defs.end(),
            [](const StreamDef& a, const StreamDef& b) { return a.streamId < b.streamId; });

  std::vector<StreamGrant> grants;
  grants.reserve(defs.size());
  for (const auto& d : defs) grants.push_back({d.streamId, 0});

  uint32_t remaining = totalBytes;
  // Pass 1: guaranteed floors. If the floors alone exceed the budget (sick
  // configuration), scale them down proportionally rather than going negative.
  uint32_t floorSum = 0;
  for (const auto& d : defs) floorSum += d.guaranteedBytes;
  if (floorSum > 0 && floorSum > totalBytes) {
    for (size_t i = 0; i < defs.size(); ++i) {
      const uint64_t g = (static_cast<uint64_t>(defs[i].guaranteedBytes) * totalBytes) / floorSum;
      grants[i].bytes = static_cast<uint32_t>(g);
    }
    return grants;
  }
  for (size_t i = 0; i < defs.size(); ++i) {
    grants[i].bytes = defs[i].guaranteedBytes;
    remaining -= defs[i].guaranteedBytes;
  }

  // Pass 2: leftover split evenly per priority level, highest first.
  // Group indices by priority (defs are in streamId order, so each group is
  // already in streamId order).
  for (int p = static_cast<int>(StreamPriority::Critical);
       p <= static_cast<int>(StreamPriority::Background) && remaining > 0; ++p) {
    std::vector<size_t> group;
    for (size_t i = 0; i < defs.size(); ++i)
      if (static_cast<int>(defs[i].priority) == p) group.push_back(i);
    if (group.empty()) continue;
    const uint32_t share = remaining / static_cast<uint32_t>(group.size());
    uint32_t rem = remaining % static_cast<uint32_t>(group.size());
    for (size_t gi : group) {
      grants[gi].bytes += share + (rem > 0 ? 1 : 0);
      if (rem > 0) --rem;
    }
    remaining = 0;  // all leftover is always fully distributed
  }
  return grants;
}

}  // namespace net
}  // namespace litholattice
