// Builds the CPU-side resource set (textures, animations, palettes, block models) from a pack
// stack. Definitions from every pack's resources.json are merged by name (higher pack wins), and
// every file is then read from the highest pack that has it, so a pack can override a single
// PNG without repeating any JSON. Schema: docs/RESOURCE_PACKS.md.
#pragma once
#include <cstdint>
#include <list>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "litholattice/assets/block_model.h"
#include "litholattice/assets/resource_pack.h"
#include "litholattice/assets/texture_registry.h"

namespace litholattice {

struct ResourceSet {
  TextureRegistry textures;
  BlockModelSet models;
  std::vector<std::string> warnings;  ///< bad entries, missing files (all non-fatal)
};

/// Never fails: anything broken becomes the missing texture plus a warning.
ResourceSet loadResources(const PackStack& stack);

/// IEEE CRC32 (the provenance hash). The assets module links neither storage nor world,
/// which carry their own copies; this one is local to pack provenance.
uint32_t lithoCrc32(std::span<const uint8_t> bytes);

/// Litho certificate: provenance marker on a pack file entry, stamped at pack-authoring
/// time as `"provenance": {"origin": "litho-generated", "crc32": "0x..."}` over the source
/// file bytes. Scope (P3-3, Appendix E L8): verified for every FILE-BACKED entry —
/// textures, animations and palettes. Model defs carry no source file, so they
/// cannot carry a verifiable marker. This is provenance ("engine-made vs
/// third-party"), NOT the mod-signing PKI (separate roadmap): CRC32 is a
/// non-cryptographic label; it tells content apart, it never gates loading.
enum class Provenance { Absent, Verified, Mismatch };
/// Verifies the "provenance" object on a file-backed manifest def. Absent when the entry
/// carries no marker (ordinary third-party content -- the default, no warning). Verified
/// when the CRC32 of the source file bytes matches the marker. Mismatch when the marker
/// is present but wrong, malformed, of unknown origin, or the source bytes can't be read;
/// `err` describes the failure. Callers treat Absent and Mismatch alike: third-party.
Provenance verifyTextureProvenance(const PackStack& stack, const std::string& rel,
                                  const Json& def, std::string& err);

/// Raster cache for SVG textures (Litho SVG): one rasterization per (CANONICAL
/// absolute path, tile size, file size, mtime), so repeated pack loads and UI
/// reuse don't re-rasterize while hot-reloaded files (mtime/size change) do.
/// The resource loader uses a shared instance; UI code may hold its own.
/// P3-3 (Appendix E L7): the cache is BOUNDED (kMaxEntries, LRU eviction),
/// MUTEX-guarded (all current callers are main-thread, but the shared
/// instance must not rely on that), and keyed by the canonical path (the
/// old raw dir_/rel key rasterised the same file twice under different
/// relative spellings). KeyHash covers path + tile + size + mtime.
struct SvgRasterCache {
  static constexpr size_t kMaxEntries = 128;
  std::optional<Image> fetch(const std::string& absPath, int tileSize);
  void store(const std::string& absPath, int tileSize, Image img);
  void clear();
  size_t size() const;
 private:
  struct Key {
    std::string path;
    int tile = 0;
    uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept;
  };
  static Key makeKey(const std::string& absPath, int tileSize);
  struct Entry {
    Image image;
    std::list<Key>::iterator lru;  // position in lru_ (front = most recent)
  };
  mutable std::mutex mutex_;
  std::unordered_map<Key, Entry, KeyHash> map_;
  mutable std::list<Key> lru_;  // front = most recently used
};

/// Clears the loader's shared SVG raster cache (test hook).
void clearSharedSvgRasterCache();

/// Current resources.json format version.
constexpr int kResourceFormatVersion = 1;

}  // namespace litholattice
