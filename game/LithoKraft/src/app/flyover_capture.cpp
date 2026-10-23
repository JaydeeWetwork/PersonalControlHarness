// flyover_capture: frame-perfect 60 fps flyover of the Wafer spawn region for docs/media.
// Fixed seed, fixed timestep (frame i = time i/60 s), software renderer: no window, no GPU,
// no screen capture, so output is identical regardless of machine load.
// Usage: flyover_capture <out.rgb> [frames=480] [W=960] [H=540]. Writes raw RGB24
// frames to <out.rgb> for ffmpeg (stdout is not used: the resource loader logs there).
// See tools/media/make_media.py.
// Based on biome_capture.cpp.
// Uses the REAL game worldgen (ColumnGenerator + wafer.json pipeline), the REAL mesher,
// and the software renderer. No window, no GPU. Fixed seed for reproducibility.
// Output: PNGs in the directory given as argv[1] (default: ./biome-shots).
// LF line endings.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "litholattice/core/math.h"
#include "litholattice/mcp/mcp_senses.h"  // encodePngRgb
#include "litholattice/render/block_materials.h"
#include "litholattice/render/mesher.h"
#include "litholattice/render/software_renderer.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/column_generator.h"
#include "litholattice/world/dimension.h"
#include "litholattice/world/mesh_input.h"
#include "lithokraft/content/blocks.h"  // blockRegistry()
#include "lithokraft/game/dimensions.h"  // createDimensions
#include "lithokraft/game/resource_manager.h"  // ResourceManager (real pack textures/materials)
#include "lithokraft/game/scout.h"  // locateBiome
#include "lithokraft/game/wafer_biomes.h"  // WaferBiome, kWaferBiomeNames
#include "lithokraft/game/map_render.h"  // renderOverviewMap, waferMapPalette

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr uint64_t kSeed = 20261011;  // fixed: reproducible biome screenshots
constexpr int kRegionChunks = 3;      // 3x3 chunk region per biome (48x48 blocks)
constexpr int kImgW = 1280, kImgH = 720;

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const size_t n = std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  return n == data.size();
}

std::string readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string s(size_t(n), '\0');
  const size_t r = std::fread(s.data(), 1, size_t(n), f);
  std::fclose(f);
  s.resize(r);
  return s;
}

// The tree features (trace_tree, capacitor_cans, resonator_spire) are wired
// into wafer.json itself (2026-10-11), so the pack file is used exactly as-is.
// (An earlier version injected duplicate feature entries in-memory; that made
// the generator invalid -- "duplicate feature name 'trace_trees'" -- and the
// game silently fell back to the built-in copy of the same file.)
std::string waferJson() {
  return readFile(std::string(LITHOKRAFT_SOURCE_DIR) + "/assets/worldgen/wafer.json");
}

litholattice::Mat4 translation(float x, float y, float z) {
  litholattice::Mat4 t = litholattice::Mat4::identity();
  t.m[12] = x;
  t.m[13] = y;
  t.m[14] = z;
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: flyover_capture <out.rgb> [frames=480] [width=960] [height=540]\n");
    return 2;
  }
  const int frames = argc > 2 ? std::max(1, std::atoi(argv[2])) : 480;
  const int W = argc > 3 ? std::max(16, std::atoi(argv[3])) : 960;
  const int H = argc > 4 ? std::max(16, std::atoi(argv[4])) : 540;
  FILE* out = std::fopen(argv[1], "wb");
  if (!out) { std::fprintf(stderr, "flyover_capture: cannot open %s\n", argv[1]); return 1; }

  // Real Wafer worldgen with fixed seed (pack wafer.json used as-is; see above).
  // Note: makeGenerator pins the wafer to seed 0, so kSeed no longer affects it.
  litholattice::WorldgenSettings settings;
  settings.seed = kSeed;
  const std::string waferJsonText = waferJson();
  if (!waferJsonText.empty()) settings.generators["wafer"] = waferJsonText;
  litholattice::DimensionRegistry reg = lithokraft::createDimensions(settings);
  litholattice::Dimension* wafer = reg.find("wafer");
  if (!wafer || !wafer->generator) {
    std::fprintf(stderr, "flyover_capture: no wafer generator\n");
    return 1;
  }
  const auto& gen = *wafer->generator;
  const litholattice::BlockRegistry& blocks = lithokraft::blockRegistry();

  // Load the real resources exactly like the game (engine base < game pack <
  // pack_64). Previously this tool built a default BlockMaterials (every face =
  // the missing texture, layer 0) and fed the renderer a single all-white tile,
  // so every screenshot came out white regardless of the actual textures.
  // The ResourceManager (and the materials it builds) must outlive all meshing.
  // Source-tree asset roots, baked in by CMake (see src/app/CMakeLists.txt).
  const std::string engineAssets = LITHOKRAFT_ENGINE_ASSETS_DIR;
  const std::string gameAssets = std::string(LITHOKRAFT_SOURCE_DIR) + "/assets";
  lithokraft::ResourceManager rm({engineAssets, gameAssets,
                                  std::filesystem::path(gameAssets) / "packs" / "pack_64"});
  rm.reload();
  const auto materialsPtr = rm.materials();
  litholattice::MeshContext mctx{blocks, *materialsPtr, nullptr};

  // Build the software renderer's texture buffer: the fragment shader samples
  // a fixed 16x16 tile per layer (texData[layer*256 + ty*16 + tx], uint32 texels
  // packed 0xAARRGGBB -- R at >>16, matching basicFragmentShader's extraction and
  // this file's framebuffer unpacking below). Box-downsample each registry
  // texture (64x64 RGBA8 in pack_64; Image pixels are R,G,B,A bytes) to 16x16.
  const litholattice::TextureRegistry& texRegistry = rm.textures();
  const uint32_t layerCount = uint32_t(texRegistry.textures().size());
  std::vector<uint32_t> texBuffer(size_t(layerCount) * 256, 0xFFFF00FFu);
  for (uint32_t layer = 0; layer < layerCount; ++layer) {
    const litholattice::Image& img =
        texRegistry.texture(litholattice::TextureId{uint16_t(layer)}).image;
    if (img.empty() || img.width() <= 0 || img.height() <= 0) continue;
    for (int y = 0; y < 16; ++y) {
      const int y0 = y * img.height() / 16, y1 = std::max(y0 + 1, (y + 1) * img.height() / 16);
      for (int x = 0; x < 16; ++x) {
        const int x0 = x * img.width() / 16, x1 = std::max(x0 + 1, (x + 1) * img.width() / 16);
        uint32_t sum[4] = {0, 0, 0, 0};
        int n = 0;
        for (int sy = y0; sy < y1; ++sy)
          for (int sx = x0; sx < x1; ++sx, ++n) {
            const uint8_t* p = img.pixel(sx, sy);
            for (int k = 0; k < 4; ++k) sum[k] += p[k];
          }
        const uint32_t r = n ? sum[0] / uint32_t(n) : 0, g = n ? sum[1] / uint32_t(n) : 0,
                       b = n ? sum[2] / uint32_t(n) : 0, a = n ? sum[3] / uint32_t(n) : 0xFF;
        texBuffer[size_t(layer) * 256 + size_t(y) * 16 + size_t(x)] =
            (a << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }
  std::printf("flyover_capture: %u texture layers loaded (%s)\n", layerCount,
              rm.stackDescription().c_str());

  constexpr int kHalf = 3;  // 7x7 chunks around spawn
  litholattice::ChunkMap map;
  map.setRegistry(&blocks);
  for (int dx = -kHalf; dx <= kHalf; ++dx)
    for (int dz = -kHalf; dz <= kHalf; ++dz) {
      auto col = gen.generate(dx, dz);
      if (col) map.insert(std::move(col));
    }
  int surfY = 64;
  for (int y = 255; y > 0; --y)
    {
      const auto id = map.get(0, y, 0);
      if (id != litholattice::kAir && id != litholattice::kAirStableId) { surfY = y; break; }
    }
  struct Mesh { std::vector<litholattice::PackedVertex> verts; float ox, oy, oz; };
  std::vector<Mesh> meshes;
  for (int dx = -kHalf; dx <= kHalf; ++dx)
    for (int dz = -kHalf; dz <= kHalf; ++dz)
      for (int sy = 0; sy < litholattice::kSubChunks; ++sy) {
        auto in = litholattice::snapshotForMeshing(map, blocks, dx, sy, dz, 0, 0);
        if (!in) continue;
        litholattice::ChunkMesh m = litholattice::meshSubChunk(*in, mctx);
        if (m.verts.empty()) continue;
        meshes.push_back(Mesh{std::move(m.verts), float(dx * 16), float(sy * 16), float(dz * 16)});
      }
  if (meshes.empty()) { std::fprintf(stderr, "flyover_capture: no meshes\n"); return 1; }
  std::fprintf(stderr, "flyover_capture: surface Y=%d, %zu sub-chunks, %d frames %dx%d @60\n",
               surfY, meshes.size(), frames, W, H);
  litholattice::SoftwareRenderer sw;
  sw.setPerPixelShading(true);
  sw.setTextures(texBuffer.data(), layerCount);
  sw.resize(W, H);
  std::vector<uint8_t> rgb(size_t(W) * size_t(H) * 3);
  for (int i = 0; i < frames; ++i) {
    // Fixed timestep: t depends only on the frame index. One full orbit per clip,
    // so the clip loops seamlessly.
    const double a = 2.0 * 3.14159265358979323846 * double(i) / double(frames);
    const float r = 44.f;
    const litholattice::Vec3 eye{float(r * std::cos(a)), float(surfY) + 26.f, float(r * std::sin(a))};
    litholattice::Vec3 fwd{-eye.x, float(surfY) - eye.y, -eye.z};
    const float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    fwd.x /= len; fwd.y /= len; fwd.z /= len;
    const litholattice::Mat4 vp = litholattice::mul(
        litholattice::perspective(float(70.0 * kDegToRad), float(W) / float(H), 0.1f, 2000.f),
        litholattice::lookDir(eye, fwd));
    sw.clear(0xFF94B3DB);  // wafer day sky
    for (const auto& m : meshes) {
      const litholattice::Mat4 mvp = litholattice::mul(vp, translation(m.ox, m.oy, m.oz));
      sw.renderMesh(m.verts.data(), m.verts.size(), mvp.m);
    }
    const auto& fb = sw.framebuffer();
    for (size_t k = 0, n = rgb.size() / 3; k < n; ++k) {
      const uint32_t p = fb.pixels[k];
      rgb[3 * k] = uint8_t((p >> 16) & 0xFF);
      rgb[3 * k + 1] = uint8_t((p >> 8) & 0xFF);
      rgb[3 * k + 2] = uint8_t(p & 0xFF);
    }
    if (std::fwrite(rgb.data(), 1, rgb.size(), out) != rgb.size()) {
      std::fprintf(stderr, "flyover_capture: write failed at frame %d\n", i);
      return 1;
    }
  }
  return std::fclose(out) == 0 ? 0 : 1;
}
