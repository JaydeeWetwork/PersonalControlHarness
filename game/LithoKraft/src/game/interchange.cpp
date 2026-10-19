#include "lithokraft/game/interchange.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "lithokraft/content/blocks.h"
#include "litholattice/sim/det_sched.h"  // detMix64

namespace lithokraft {

namespace {

// Deterministic roll stream for one region (splitmix-style over detMix64).
struct Rolls {
  uint64_t s;
  explicit Rolls(uint64_t state) : s(state) {}
  uint64_t next() {
    s += 0x9E3779B97F4A7C15ULL;
    return litholattice::detMix64(s);
  }
  int below(int n) { return int(next() % uint64_t(n)); }
};

uint64_t regionState(uint64_t seed, int rx, int rz) {
  uint64_t h = litholattice::detMix64(seed ^ 0x1CE7C0FFEE123456ULL);
  h = litholattice::detMix64(h ^ (uint64_t(int64_t(rx)) * 0x9E3779B97F4A7C15ULL));
  h = litholattice::detMix64(h ^ (uint64_t(int64_t(rz)) * 0xC2B2AE3D27D4EB4FULL));
  return h;
}

// Rotate a local offset by rot * 90 degrees (additive mod 4, see header).
void rotXZ(int lx, int lz, int rot, int& ox, int& oz) {
  switch (rot & 3) {
    case 0: ox = lx; oz = lz; break;
    case 1: ox = lz; oz = -lx; break;
    case 2: ox = -lx; oz = -lz; break;
    default: ox = -lz; oz = lx; break;
  }
}

// Facing rotation for a building at ring offset (ox, oz) so its local -Z
// front points at the plaza centre (dominant axis wins).
int facingRot(int ox, int oz) {
  if (std::abs(ox) >= std::abs(oz)) return ox > 0 ? 1 : 3;  // front -> -X / +X
  return oz > 0 ? 0 : 2;                                    // front -> -Z / +Z
}

float yawToward(int fromX, int fromZ, int toX, int toZ) {
  // Engine convention (buildDemoCircuit spawn): forward = (cos yaw, sin yaw)
  // in XZ — yaw -pi/2 looks along -Z.
  return std::atan2(float(toZ - fromZ), float(toX - fromX));
}

bool footprintHitsReserve(int cx, int cz) {
  using T = InterchangeTuning;
  const int nx = std::clamp(cx, T::kReserveMin, T::kReserveMax - 1);
  const int nz = std::clamp(cz, T::kReserveMin, T::kReserveMax - 1);
  const int dx = cx - nx, dz = cz - nz;
  return dx * dx + dz * dz < T::kFootprintRadius * T::kFootprintRadius;
}

// Ring layout in site-local space (before the site rotation): kind +
// offset. Buildings stay inside the pad disc (worst corner ~22.4 < 24).
struct RingSlot { const char* kind; int ox, oz; };
constexpr RingSlot kRing[] = {
    {"house_a", -16, -11}, {"house_b", 15, -13}, {"stall", -15, 12},
    {"stall", 14, 13},     {"workshop", 1, -20}, {"house_a", -20, 1},
};

void halfExtents(const char* kind, int& hw, int& hd) {
  if (std::string(kind) == "house_a") { hw = 2; hd = 2; }
  else if (std::string(kind) == "house_b") { hw = 2; hd = 3; }
  else if (std::string(kind) == "stall") { hw = 1; hd = 1; }
  else { hw = 3; hd = 2; }  // workshop
}

}  // namespace

InterchangePlan planInterchange(uint64_t worldSeed, int regionX, int regionZ) {
  using T = InterchangeTuning;
  InterchangePlan plan;
  plan.regionX = regionX;
  plan.regionZ = regionZ;
  Rolls rolls(regionState(worldSeed, regionX, regionZ));
  if (rolls.below(8) >= T::kPresenceOutOf8) return plan;  // no site in this region
  plan.centreX = regionX * T::kRegionSize + 32 + rolls.below(64);
  plan.centreZ = regionZ * T::kRegionSize + 32 + rolls.below(64);
  if (footprintHitsReserve(plan.centreX, plan.centreZ)) return plan;  // home reserve
  plan.rotation = rolls.below(4);
  plan.mix = rolls.below(8) < 5 ? InterchangeMix::ReceiptorHeavy : InterchangeMix::DeceitorHeavy;
  plan.present = true;

  // Buildings: ring offsets rotated by the site rotation; each building
  // faces the plaza.
  for (const RingSlot& slot : kRing) {
    BuildingPlan b;
    b.kind = slot.kind;
    int wx, wz;
    rotXZ(slot.ox, slot.oz, plan.rotation, wx, wz);
    b.cx = plan.centreX + wx;
    b.cz = plan.centreZ + wz;
    b.rot = (facingRot(slot.ox, slot.oz) + plan.rotation) & 3;
    halfExtents(b.kind, b.halfW, b.halfD);
    plan.buildings.push_back(b);
  }

  // Population anchors (world x/z; y stays pad-relative feet = 1).
  std::vector<PopSlot> rcAnchors, dcAnchors, netorAnchors;
  auto anchor = [&](std::vector<PopSlot>& dst, PopKind kind, int bx, int bz, int lx, int lz, int rot,
                    const char* role, int building, int faceX, int faceZ) {
    int ox, oz;
    rotXZ(lx, lz, rot, ox, oz);
    PopSlot s;
    s.kind = kind;
    s.x = bx + ox;
    s.z = bz + oz;
    s.y = 1;  // pad-relative feet (padY + 1 after build)
    s.yaw = yawToward(s.x, s.z, faceX, faceZ);
    s.role = role;
    s.building = building;
    dst.push_back(s);
  };
  for (size_t i = 0; i < plan.buildings.size(); ++i) {
    const BuildingPlan& b = plan.buildings[i];
    const std::string kind = b.kind;
    if (kind == "house_a" || kind == "house_b") {
      anchor(rcAnchors, PopKind::Receiptor, b.cx, b.cz, 0, -b.halfD - 2, b.rot, "resident", int(i),
             b.cx, b.cz);
      anchor(dcAnchors, PopKind::Deceitor, b.cx, b.cz, 0, b.halfD + 2, b.rot, "lurker", int(i),
             plan.centreX, plan.centreZ);
    } else if (kind == "stall") {
      anchor(rcAnchors, PopKind::Receiptor, b.cx, b.cz, 0, 1, b.rot, "trader", int(i), b.cx, b.cz);
      anchor(dcAnchors, PopKind::Deceitor, b.cx, b.cz, b.halfW + 2, 0, b.rot, "lurker", int(i),
             plan.centreX, plan.centreZ);
    } else {  // workshop
      anchor(rcAnchors, PopKind::Receiptor, b.cx, b.cz, 0, -b.halfD - 2, b.rot, "keeper", int(i),
             b.cx, b.cz);
      anchor(dcAnchors, PopKind::Deceitor, b.cx, b.cz, 0, b.halfD + 2, b.rot, "lurker", int(i),
             plan.centreX, plan.centreZ);
    }
  }
  // Second RC pass: one resident indoors per house.
  for (size_t i = 0; i < plan.buildings.size(); ++i) {
    const BuildingPlan& b = plan.buildings[i];
    if (std::string(b.kind) == "house_a" || std::string(b.kind) == "house_b")
      anchor(rcAnchors, PopKind::Receiptor, b.cx, b.cz, 0, 0, b.rot, "resident", int(i),
             plan.centreX, plan.centreZ);
  }
  // Plaza-edge lurkers + Netor tidiers (building -1 = plaza).
  anchor(dcAnchors, PopKind::Deceitor, plan.centreX, plan.centreZ, 11, 5, plan.rotation, "lurker",
         -1, plan.centreX, plan.centreZ);
  anchor(dcAnchors, PopKind::Deceitor, plan.centreX, plan.centreZ, -11, -6, plan.rotation, "lurker",
         -1, plan.centreX, plan.centreZ);
  anchor(netorAnchors, PopKind::Netor, plan.centreX, plan.centreZ, 4, 3, plan.rotation, "tidier",
         -1, plan.centreX, plan.centreZ);
  anchor(netorAnchors, PopKind::Netor, plan.centreX, plan.centreZ, -4, -3, plan.rotation, "tidier",
         -1, plan.centreX, plan.centreZ);
  anchor(netorAnchors, PopKind::Netor, plan.centreX, plan.centreZ, 3, -5, plan.rotation, "tidier",
         -1, plan.centreX, plan.centreZ);

  plan.countReceiptor = plan.mix == InterchangeMix::ReceiptorHeavy ? T::kRcHeavyRc : T::kDcHeavyRc;
  plan.countDeceitor = plan.mix == InterchangeMix::ReceiptorHeavy ? T::kRcHeavyDc : T::kDcHeavyDc;
  plan.countNetor = plan.mix == InterchangeMix::ReceiptorHeavy ? T::kRcHeavyNetor : T::kDcHeavyNetor;
  auto fill = [&](const std::vector<PopSlot>& anchors, int count) {
    for (int i = 0; i < count && !anchors.empty(); ++i) {
      PopSlot s = anchors[size_t(i) % anchors.size()];
      if (i >= int(anchors.size())) s.x += i / int(anchors.size());  // cycle without stacking
      plan.population.push_back(s);
    }
  };
  fill(rcAnchors, plan.countReceiptor);
  fill(dcAnchors, plan.countDeceitor);
  fill(netorAnchors, plan.countNetor);
  return plan;
}

std::vector<InterchangePlan> planInterchangesIn(uint64_t worldSeed, int minX, int minZ, int maxX,
                                                int maxZ) {
  using T = InterchangeTuning;
  std::vector<InterchangePlan> out;
  const int rx0 = (minX - T::kFootprintRadius) / T::kRegionSize - 1;
  const int rz0 = (minZ - T::kFootprintRadius) / T::kRegionSize - 1;
  const int rx1 = (maxX + T::kFootprintRadius) / T::kRegionSize + 1;
  const int rz1 = (maxZ + T::kFootprintRadius) / T::kRegionSize + 1;
  for (int rz = rz0; rz <= rz1; ++rz)
    for (int rx = rx0; rx <= rx1; ++rx) {
      InterchangePlan p = planInterchange(worldSeed, rx, rz);
      if (!p.present) continue;
      const int dx = std::max({minX - p.centreX, 0, p.centreX - (maxX - 1)});
      const int dz = std::max({minZ - p.centreZ, 0, p.centreZ - (maxZ - 1)});
      if (dx * dx + dz * dz <= T::kFootprintRadius * T::kFootprintRadius) out.push_back(p);
    }
  return out;
}

// ------------------------------------------------------------ building
namespace {

struct Stamp {
  litholattice::ChunkMap& w;
  int placed = 0;
  void put(int x, int y, int z, BlockId b) {
    if (y < 0 || y >= litholattice::kWorldHeight) return;
    if (w.get(x, y, z) != b && w.set(x, y, z, b)) ++placed;
  }
};

// One building in local space (front = -Z, ly 0 = first block above the
// pad top, floor at ly -1), rotated by the building's rot.
struct BuildingStamp {
  Stamp& stamp;
  int cx, cz, baseY, rot;  // baseY = padY + 1
  void put(int lx, int ly, int lz, BlockId b) {
    int ox, oz;
    rotXZ(lx, lz, rot, ox, oz);
    stamp.put(cx + ox, baseY + ly, cz + oz, b);
  }
};

void buildHouse(BuildingStamp& bs, bool variantB) {
  const int hw = 2, hd = variantB ? 3 : 2;
  const BlockId wall = variantB ? kCeramic : kWaferSilicon;
  const BlockId trim = variantB ? kWaferSilicon : kCeramic;
  const BlockId glass = variantB ? kFrostedGlass : kGlass;
  const BlockId floor = variantB ? kDeadSilicon : kCeramic;
  for (int x = -hw; x <= hw; ++x)
    for (int z = -hd; z <= hd; ++z) bs.put(x, -1, z, floor);
  for (int ly = 0; ly <= 2; ++ly)
    for (int x = -hw; x <= hw; ++x)
      for (int z = -hd; z <= hd; ++z) {
        if (x != -hw && x != hw && z != -hd && z != hd) continue;  // perimeter only
        if (z == -hd && x == 0 && ly <= 1) continue;               // door
        if (ly == 1 && ((z == -hd && (x == -1 || x == 1)) || (z == hd && x == 0) ||
                        (x == -hw && z == 0) || (x == hw && z == 0))) {
          bs.put(x, ly, z, glass);
          continue;
        }
        bs.put(x, ly, z, ly == 2 ? trim : wall);
      }
  for (int x = -hw; x <= hw; ++x)
    for (int z = -hd; z <= hd; ++z) bs.put(x, 3, z, variantB ? kStoneSlab : kSiliconSlab);
  bs.put(0, 2, 0, kLamp);  // ceiling lamp under the roof
}

void buildStall(BuildingStamp& bs) {
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z) bs.put(x, -1, z, kCeramic);
  for (int sx : {-1, 1})
    for (int sz : {-1, 1}) {
      bs.put(sx, 0, sz, kTraceTrunk);
      bs.put(sx, 1, sz, kTraceTrunk);
    }
  for (int x = -1; x <= 1; ++x) bs.put(x, 0, -1, kCeramic);  // counter (front)
  bs.put(-1, 1, -1, kChipOR);                               // goods on the counter
  bs.put(1, 1, -1, kColorLamp16);                           // glowing display piece
  for (int x = -2; x <= 2; ++x)
    for (int z = -2; z <= 2; ++z) bs.put(x, 2, z, kSiliconSlab);  // canopy
}

void buildWorkshop(BuildingStamp& bs) {
  const int hw = 3, hd = 2;
  for (int x = -hw; x <= hw; ++x)
    for (int z = -hd; z <= hd; ++z) bs.put(x, -1, z, kDenseSilicon);
  for (int ly = 0; ly <= 2; ++ly)
    for (int x = -hw; x <= hw; ++x)
      for (int z = -hd; z <= hd; ++z) {
        if (x != -hw && x != hw && z != -hd && z != hd) continue;
        if (z == -hd && x == 0 && ly <= 1) continue;  // door
        if (ly == 1 && ((z == -hd && (x == -2 || x == 2)) || (z == hd && (x == -2 || x == 2)) ||
                        (x == -hw && z == 0) || (x == hw && z == 0))) {
          bs.put(x, ly, z, kFrostedGlass);
          continue;
        }
        bs.put(x, ly, z, kStone);
      }
  for (int x = -2; x <= 0; ++x) bs.put(x, 0, 1, kDeadMemoryCell);  // memory bank along the back
  bs.put(2, 0, 1, kChipOR);
  bs.put(2, 1, 1, kChipOR);  // a small stack of printed chips
  for (int x = -hw; x <= hw; ++x)
    for (int z = -hd; z <= hd; ++z) bs.put(x, 3, z, kStoneSlab);
  bs.put(0, 4, 0, kLamp);  // roof beacon
}

}  // namespace

int buildInterchange(litholattice::ChunkMap& w, const InterchangePlan& plan,
                     BuiltInterchange* out) {
  using T = InterchangeTuning;
  if (!plan.present) return 0;
  const int padY = w.surfaceY(plan.centreX, plan.centreZ);
  if (padY < 4 || padY + 12 >= litholattice::kWorldHeight) return 0;
  Stamp stamp{w, 0};

  // Level the die-pad: cut above padY, fill below with wafer silicon.
  const int R = T::kPadRadius;
  for (int dz = -R; dz <= R; ++dz)
    for (int dx = -R; dx <= R; ++dx) {
      if (dx * dx + dz * dz > R * R) continue;
      const int x = plan.centreX + dx, z = plan.centreZ + dz;
      const int sy = w.surfaceY(x, z);
      if (sy < 0) continue;  // ungenerated column: nothing to level
      if (sy > padY)
        for (int y = padY + 1; y <= sy; ++y) stamp.put(x, y, z, litholattice::kAir);
      else if (sy < padY)
        for (int y = sy + 1; y <= padY; ++y) stamp.put(x, y, z, kWaferSilicon);
    }

  // Plaza paving + the centre lamp feature.
  for (int dz = -T::kPlazaRadius; dz <= T::kPlazaRadius; ++dz)
    for (int dx = -T::kPlazaRadius; dx <= T::kPlazaRadius; ++dx)
      if (dx * dx + dz * dz <= T::kPlazaRadius * T::kPlazaRadius)
        stamp.put(plan.centreX + dx, padY, plan.centreZ + dz, kScribeSilicon);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dz = -1; dz <= 1; ++dz)
      stamp.put(plan.centreX + dx, padY, plan.centreZ + dz, kDenseSilicon);
  for (int ly = 1; ly <= 3; ++ly) stamp.put(plan.centreX, padY + ly, plan.centreZ, kTraceTrunk);
  stamp.put(plan.centreX, padY + 4, plan.centreZ, kLamp);
  for (int sx : {-6, 6})
    for (int sz : {-6, 6}) {
      stamp.put(plan.centreX + sx, padY + 1, plan.centreZ + sz, kStone);
      stamp.put(plan.centreX + sx, padY + 2, plan.centreZ + sz, kLamp);
    }

  // Paved strips from the plaza edge to each building's door.
  for (const BuildingPlan& b : plan.buildings) {
    int fox, foz;
    rotXZ(0, -b.halfD - 1, b.rot, fox, foz);
    const int doorX = b.cx + fox, doorZ = b.cz + foz;
    const double len = std::hypot(double(doorX - plan.centreX), double(doorZ - plan.centreZ));
    const int steps = int(len * 2.0);
    for (int i = 0; i <= steps; ++i) {
      const double t = steps ? double(i) / steps : 0.0;
      const int px = int(std::lround(plan.centreX + (doorX - plan.centreX) * t));
      const int pz = int(std::lround(plan.centreZ + (doorZ - plan.centreZ) * t));
      const int distC = (px - plan.centreX) * (px - plan.centreX) +
                        (pz - plan.centreZ) * (pz - plan.centreZ);
      if (distC <= T::kPlazaRadius * T::kPlazaRadius) continue;  // plaza already paved
      for (int ax = -1; ax <= 1; ++ax)
        for (int az = -1; az <= 1; ++az) stamp.put(px + ax, padY, pz + az, kFleckedSilicon);
    }
  }

  // The building ring.
  for (const BuildingPlan& b : plan.buildings) {
    BuildingStamp bs{stamp, b.cx, b.cz, padY + 1, b.rot};
    const std::string kind = b.kind;
    if (kind == "house_a") buildHouse(bs, false);
    else if (kind == "house_b") buildHouse(bs, true);
    else if (kind == "stall") buildStall(bs);
    else buildWorkshop(bs);
  }

  if (out) {
    out->plan = plan;
    out->padY = padY;
    out->population = plan.population;
    for (PopSlot& s : out->population) s.y = padY + s.y;  // pad-relative -> world feet
    out->blocksPlaced = stamp.placed;
  }
  return stamp.placed;
}

int placeInterchanges(litholattice::ChunkMap& w, uint64_t worldSeed, int sizeChunks,
                      std::vector<BuiltInterchange>* built) {
  using T = InterchangeTuning;
  const int extent = sizeChunks * 16;
  int count = 0;
  for (const InterchangePlan& plan : planInterchangesIn(worldSeed, 0, 0, extent, extent)) {
    // Footprint (pad + margin) must be fully inside the loaded world.
    if (plan.centreX - T::kPadRadius - 4 < 0 || plan.centreZ - T::kPadRadius - 4 < 0 ||
        plan.centreX + T::kPadRadius + 4 >= extent || plan.centreZ + T::kPadRadius + 4 >= extent)
      continue;
    BuiltInterchange b;
    if (buildInterchange(w, plan, built ? &b : nullptr) <= 0) continue;
    if (built) built->push_back(std::move(b));
    ++count;
  }
  return count;
}

namespace {

const char* kindName(PopKind k) {
  switch (k) {
    case PopKind::Receiptor: return "receiptor";
    case PopKind::Deceitor: return "deceitor";
    case PopKind::Netor: return "netor";
  }
  return "?";
}

std::string jsonEscape(const char* s) { return s; }  // our strings are fixed literals

}  // namespace

std::string interchangeManifestJson(const BuiltInterchange& built) {
  const InterchangePlan& p = built.plan;
  std::string out;
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "{\n  \"generator\": \"lithokraft-interchange v1\",\n"
                "  \"region\": [%d, %d],\n  \"centre\": [%d, %d],\n  \"padY\": %d,\n"
                "  \"rotation\": %d,\n  \"mix\": \"%s\",\n  \"counts\": {\"receiptor\": %d, "
                "\"deceitor\": %d, \"netor\": %d},\n  \"buildings\": [",
                p.regionX, p.regionZ, p.centreX, p.centreZ, built.padY, p.rotation,
                p.mix == InterchangeMix::ReceiptorHeavy ? "receiptor_heavy" : "deceitor_heavy",
                p.countReceiptor, p.countDeceitor, p.countNetor);
  out += buf;
  bool first = true;
  for (const BuildingPlan& b : p.buildings) {
    std::snprintf(buf, sizeof buf, "%s\n    {\"kind\": \"%s\", \"centre\": [%d, %d], \"rot\": %d}",
                  first ? "" : ",", jsonEscape(b.kind).c_str(), b.cx, b.cz, b.rot);
    out += buf;
    first = false;
  }
  out += "\n  ],\n  \"population\": [";
  first = true;
  for (const PopSlot& s : built.population) {
    std::snprintf(buf, sizeof buf,
                  "%s\n    {\"kind\": \"%s\", \"role\": \"%s\", \"feet\": [%d, %d, %d], "
                  "\"yaw\": %.4f, \"building\": %d}",
                  first ? "" : ",", kindName(s.kind), jsonEscape(s.role).c_str(), s.x, s.y, s.z,
                  double(s.yaw), s.building);
    out += buf;
    first = false;
  }
  out += "\n  ]\n}\n";
  return out;
}

}  // namespace lithokraft
