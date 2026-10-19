#include "litholattice/storage/block_names.h"
#include "litholattice/storage/file_contract.h"
#include "litholattice/storage/storage.h"

namespace litholattice::storage {
namespace {
constexpr uint32_t kMagic = 0x4E424C4C;  // "LLBN"

// Fail-closed intactness check for recoverCrashedWrite, modelled on
// RegionFile's regionImageIntact: a generation counts as intact only when
// the WHOLE image parses (magic + version + entry count + every entry in
// bounds + exact end-of-body + trailing CRC). Anything less is torn.
bool blockNamesIntact(const std::filesystem::path& p, std::string* err) {
  Bytes b;
  if (!readFileAll(p, &b, err)) return false;
  auto fail = [&](const char* why) { if (err) *err = p.string() + ": " + why; return false; };
  if (b.size() < 16) return fail("too short");
  ByteReader crcR(b.data() + b.size() - 4, 4);
  if (crcR.u32() != crc32(b.data(), b.size() - 4)) return fail("CRC mismatch (damaged block name table)");
  ByteReader rd(b.data(), b.size() - 4);
  if (rd.u32() != kMagic) return fail("not a block name table");
  if (rd.u32() != kBlockNamesVersion) return fail("unsupported block name table version");
  const uint32_t n = rd.u32();
  if (n > 65536) return fail("too many entries");
  for (uint32_t i = 0; i < n && rd.ok(); ++i) {
    const uint16_t len = rd.u16();
    for (uint16_t k = 0; k < len && rd.ok(); ++k) rd.u8();
  }
  if (!rd.ok() || !rd.done()) return fail("truncated");
  return true;
}

// True when `file` or any crash footprint of it (backup generation / stale
// temp) exists. No footprint at all = genuinely absent = the legacy path.
bool tableFootprintExists(const std::filesystem::path& file) {
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) return true;
  if (std::filesystem::exists(std::filesystem::path(file.string() + kAtomicTmpSuffix), ec)) return true;
  for (unsigned i = 1; i <= kBackupDepthMax; ++i)
    if (std::filesystem::exists(std::filesystem::path(file.string() + kBackupSuffix + "." + std::to_string(i)), ec)) return true;
  return false;
}
}  // namespace

bool writeBlockNames(const std::filesystem::path& file, const BlockRegistry& blocks, std::string* err) {
  ByteWriter w;
  w.u32(kMagic);
  w.u32(kBlockNamesVersion);
  w.u32(uint32_t(blocks.size()));
  for (size_t i = 0; i < blocks.size(); ++i) {
    const std::string id = blocks.idOf(BlockId(i));
    w.u16(uint16_t(id.size()));
    for (char c : id) w.u8(uint8_t(c));
  }
  w.u32(crc32(w.out.data(), w.out.size()));
  // C-4: the standard file contract (fsync file + directory, backup rotation
  // at the default depth, temp hygiene) — not the old hand-rolled tmp+rename,
  // which had no fsync and kept no backup of the file that interprets all
  // region data.
  return writeFileAtomic(file, w.out, err);
}

std::optional<BlockRemap> readBlockRemap(const std::filesystem::path& file, const BlockRegistry& blocks, std::string* err) {
  BlockRemap r;
  if (!tableFootprintExists(file)) {
    r.legacy = true;
    for (size_t i = 0; i < blocks.size(); ++i) r.map.push_back(BlockId(i));
    return r;
  }
  // Crash recovery before reading: a torn primary heals from the newest
  // intact backup (or a fully-written temp). A footprint with NO intact
  // generation fails closed here — nullopt + err, never "legacy".
  {
    std::string rerr;
    if (!recoverCrashedWrite(file, blockNamesIntact, &rerr)) {
      if (err) *err = rerr.empty() ? (file.string() + ": block name table unreadable") : rerr;
      return std::nullopt;
    }
  }
  Bytes b;
  if (!readFileAll(file, &b, err)) return std::nullopt;
  auto fail = [&](const char* why) -> std::optional<BlockRemap> { if (err) *err = file.string() + ": " + why; return std::nullopt; };
  if (b.size() < 16) return fail("too short");
  ByteReader crcR(b.data() + b.size() - 4, 4);
  if (crcR.u32() != crc32(b.data(), b.size() - 4)) return fail("CRC mismatch (damaged block name table)");
  ByteReader rd(b.data(), b.size() - 4);
  if (rd.u32() != kMagic) return fail("not a block name table");
  if (rd.u32() != kBlockNamesVersion) return fail("unsupported block name table version");
  const uint32_t n = rd.u32();
  if (n > 65536) return fail("too many entries");
  for (uint32_t i = 0; i < n && rd.ok(); ++i) {
    const uint16_t len = rd.u16();
    std::string id;
    for (uint16_t k = 0; k < len && rd.ok(); ++k) id.push_back(char(rd.u8()));
    const auto cur = blocks.findById(id);
    if (!cur) r.unknown.push_back(id);
    const BlockId to = cur ? *cur : kAir;
    if (to != BlockId(i)) r.identity = false;
    r.map.push_back(to);
  }
  if (!rd.ok() || !rd.done()) return fail("truncated");
  return r;
}

std::optional<StableRemap> readStableRemap(const std::filesystem::path& file, const BlockRegistry& blocks,
                                           std::string* err) {
  auto base = readBlockRemap(file, blocks, err);
  if (!base) return std::nullopt;
  StableRemap r;
  r.unknown = std::move(base->unknown);
  r.legacy = base->legacy;
  r.identity = base->identity;
  r.map.reserve(base->map.size());
  for (BlockId id : base->map) r.map.push_back(blocks.stableIdOf(id));
  return r;
}

bool remapColumn(ChunkColumn& c, const StableRemap& remap, std::string* err) {
  for (auto& sub : c.subs) {
    if (!sub || !sub->loadScratch) continue;  // fresh worldgen: voxels already installed
    const auto& scratch = *sub->loadScratch;
    if (scratch.size() != kSubChunkVolume) {
      if (err) *err = "block load scratch has wrong size";
      return false;
    }
    uint16_t nonAir = 0;
    for (size_t i = 0; i < scratch.size(); ++i) {
      const uint16_t saveId = scratch[i];
      const StableId sid = saveId < remap.map.size() ? remap.map[saveId] : kAirStableId;
      sub->blocks.set(i, sid);
      if (!isAirStableId(sid)) ++nonAir;
    }
    sub->nonAir = nonAir;
    sub->loadScratch.reset();
  }
  return true;
}

}  // namespace litholattice::storage
