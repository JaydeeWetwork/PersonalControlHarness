#include "lithokraft/game/resource_manager.h"
#include "litholattice/render/hud_ui_renderer.h"
#include <iterator>
#include <string>
#include "lithokraft/content/appearance.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/items.h"
#include "litholattice/core/log.h"
#include "litholattice/render/renderer.h"
#include "litholattice/render/texture_array.h"

namespace lithokraft {

ResourceManager::ResourceManager(std::vector<std::filesystem::path> packDirs)
    : dirs_(std::move(packDirs)), materials_(std::make_shared<const BlockMaterials>()) {}

void ResourceManager::reload() {
  PackStack stack;
  std::string text;
  for (const auto& dir : dirs_) {
    std::string err;
    if (auto pack = ResourcePack::open(dir, &err)) {
      text += (text.empty() ? "" : " < ") + pack->name();
      stack.push(std::move(*pack));
    } else {
      logError("resources: pack skipped: %s", err.c_str());
    }
  }
  // Hotbar icons. (They used to be flat material swatches: the HUD never read textures/ui/icons.)
  std::vector<Image> icons(blockRegistry().size());
  size_t missingIcons = 0;
  for (size_t b = 1; b < icons.size(); ++b) {
    std::string err;
    const auto file = stack.find("textures/ui/icons/" + iconName(BlockId(b)) + ".png");
    if (auto img = file ? Image::load(*file, &err) : std::nullopt) icons[b] = downsampleIcon(*img, 16);
    else ++missingIcons;
  }
  if (missingIcons) logError("resources: %zu block icon(s) missing under textures/ui/icons (swatches used)", missingIcons);
  icons_ = std::move(icons);
  // Item icons (non-block items from the item registry: tools, the Fragment,
  // the seed). Same pack path + 16x16 box filter as block icons. Block-backed
  // items (ChipOR) keep using icon(BlockId) and are skipped here.
  std::map<uint32_t, Image> itemIcons;
  size_t missingItemIcons = 0;
  for (const ItemDef& def : itemRegistry()) {
    if (def.itemId < kFirstItemId) continue;  // block-backed
    std::string err;
    const auto file = stack.find("textures/ui/icons/" + std::string(def.iconName) + ".png");
    if (auto img = file ? Image::load(*file, &err) : std::nullopt) itemIcons[def.itemId] = downsampleIcon(*img, 16);
    else ++missingItemIcons;
  }
  if (missingItemIcons) logError("resources: %zu item icon(s) missing under textures/ui/icons", missingItemIcons);
  itemIcons_ = std::move(itemIcons);
  ResourceSet next = loadResources(stack);
  std::vector<std::string> warnings = std::move(next.warnings);
  ShaderLibrary shaders = ShaderLibrary::load(stack, kShaderPrograms, &warnings);
  auto materials = std::make_shared<const BlockMaterials>(BlockMaterials::build(blockRegistry(), next.textures, next.models, &warnings, &blockAppearance()));
  std::map<std::string, std::string, std::less<>> worldgen;
  for (const std::string_view id : kWorldgenDimensions) {
    std::string origin;
    if (auto json = stack.readText("worldgen/" + std::string(id) + ".json", &origin)) {
      worldgen.emplace(id, std::move(*json));
      logInfo("  resources: worldgen/%s.json from %s", std::string(id).c_str(), origin.c_str());
    }
  }

  ui::Theme theme = ui::Theme::defaults();
  {
    std::string origin, err;
    if (auto json = stack.readText("ui/theme.json", &origin)) {
      if (ui::Theme::parse(*json, theme, err)) logInfo("  resources: ui/theme.json from %s", origin.c_str());
      else warnings.push_back("ui/theme.json (" + origin + "): " + err + " - using the built-in theme");
    }
  }

  logInfo("resources: %s | %zu textures (%zu missing), %zu animations, %zu palettes, %zu models, %zu/%zu shader programs from packs",
          text.c_str(), next.textures.textures().size(), next.textures.missingNames().size(),
          next.textures.animations().size(), next.textures.palettes().size(), next.models.size(), shaders.size(),
          std::size(kShaderPrograms));
  for (const auto& w : warnings) logInfo("  resources: %s", w.c_str());
  if (loaded_) {
    logInfo("hot reload: %s; shaders %s; models %zu -> %zu", diffRegistries(resources_.textures, next.textures).summary().c_str(),
            shaders == shaders_ ? "unchanged" : "changed", resources_.models.size(), next.models.size());
  }
  resources_ = std::move(next);
  shaders_ = std::move(shaders);
  stackText_ = std::move(text);
  worldgenSources_ = std::move(worldgen);
  theme_ = std::move(theme);
  ++themeRevision_;
  loaded_ = true;
  std::lock_guard lock(mutex_);
  materials_ = std::move(materials);
}

void ResourceManager::apply(IRenderer& renderer) const {
  if (!renderer.textureArray().upload(resources_.textures)) logError("resources: texture array upload reported a GL error");
  renderer.setMaterials(*materials());
  renderer.loadShaders(shaders_);
}

WorldgenSettings ResourceManager::worldgen(uint64_t seed) const {
  WorldgenSettings s;
  s.seed = seed;
  s.generators = worldgenSources_;
  return s;
}

std::shared_ptr<const BlockMaterials> ResourceManager::materials() const {
  std::lock_guard lock(mutex_);
  return materials_;
}

const Image* ResourceManager::itemIcon(uint32_t itemId) const {
  const auto it = itemIcons_.find(itemId);
  return it != itemIcons_.end() && !it->second.empty() ? &it->second : nullptr;
}

std::string ResourceManager::iconName(BlockId b) {
  const std::string tex(blockRegistry().def(b).tex);
  return tex == "grass" ? "grass_block" : tex;  // the art's icon file names follow the block names
}

}  // namespace lithokraft
