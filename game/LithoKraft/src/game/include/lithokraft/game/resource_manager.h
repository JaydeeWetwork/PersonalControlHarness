// ResourceManager: owns the loaded resource packs and everything built from them (texture
// registry, block models, BlockMaterials, shader sources, world generator JSON, UI theme), pushes them to
// the renderer, and rebuilds all of it on a debug hot reload (F9). Worldgen sources are re-read
// on reload but only take effect for worlds created afterwards (a new game).
#pragma once
#include "litholattice/assets/image.h"
#include "lithokraft/game/engine.h"
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/assets/resource_loader.h"
#include "litholattice/assets/shader_library.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/ui/theme.h"
#include "litholattice/world/column_generator.h"

namespace litholattice {
class IRenderer;
}  // namespace litholattice

namespace lithokraft {

class ResourceManager {
 public:
  /// `packDirs`: lowest priority first (engine base, game pack, art pack, mods, player pack).
  explicit ResourceManager(std::vector<std::filesystem::path> packDirs);

  /// Reads every pack again and rebuilds the CPU side; logs warnings and, after the first load,
  /// what changed. Main thread. Packs that fail to open are skipped (logged).
  void reload();
  /// Uploads the texture array, material uniforms and shaders to `renderer` (GL thread).
  void apply(IRenderer& renderer) const;

  /// Current materials, safe to call from any thread (mesh jobs are submitted by the sim
  /// thread); a job keeps the set it started with alive across a reload.
  std::shared_ptr<const BlockMaterials> materials() const;
  const TextureRegistry& textures() const { return resources_.textures; }
  const BlockModelSet& models() const { return resources_.models; }
  const ShaderLibrary& shaders() const { return shaders_; }
  /// Hotbar / inventory icon of `b` (textures/ui/icons/<name>.png from the highest pack, box-
  /// filtered to 16 x 16), or nullptr when no pack has one. Main thread.
  const Image* icon(BlockId b) const { return b < icons_.size() && !icons_[b].empty() ? &icons_[b] : nullptr; }
  /// The icon file name of `b`: its texture name, except where the art names it differently.
  static std::string iconName(BlockId b);
  /// Hotbar / inventory icon of a non-block item (ItemDef::iconName from the
  /// item registry, textures/ui/icons/<name>.png, box-filtered to 16 x 16),
  /// or nullptr when no pack has one / the id is not a registered item.
  /// Block-backed items (e.g. ChipOR) resolve through icon(BlockId) instead.
  const Image* itemIcon(uint32_t itemId) const;
  /// World seed + generator JSON from the packs (worldgen/<dimension>.json, highest pack wins)
  /// for createDimensions() (dimensions.h).
  WorldgenSettings worldgen(uint64_t seed) const;
  /// UI theme from ui/theme.json (highest pack wins; the built-in theme if none or invalid).
  const ui::Theme& theme() const { return theme_; }
  /// Bumped by every reload (lets the UI re-apply the theme after F9).
  int themeRevision() const { return themeRevision_; }
  /// Names of the packs in the stack, lowest first ("base < pack_64").
  const std::string& stackDescription() const { return stackText_; }

  /// Programs the renderer expects in shaders/.
  static constexpr std::string_view kShaderPrograms[] = {"chunk", "hud", "line"};
  /// Dimensions whose generator a pack may replace with worldgen/<id>.json.
  static constexpr std::string_view kWorldgenDimensions[] = {"wafer", "organic"};

 private:
  std::vector<std::filesystem::path> dirs_;
  ResourceSet resources_;
  ShaderLibrary shaders_;
  std::string stackText_;
  std::map<std::string, std::string, std::less<>> worldgenSources_;  // dimension id -> JSON
  ui::Theme theme_ = ui::Theme::defaults();
  int themeRevision_ = 0;
  bool loaded_ = false;
  mutable std::mutex mutex_;
  std::shared_ptr<const BlockMaterials> materials_;
  std::vector<Image> icons_;  // by BlockId
  std::map<uint32_t, Image> itemIcons_;  // by non-block itemId (item registry)
};

}  // namespace lithokraft
