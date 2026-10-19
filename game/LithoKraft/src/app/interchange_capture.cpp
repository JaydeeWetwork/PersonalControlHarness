// interchange_capture: headless screenshots of a generated Wafer
// interchange (JD directive, 2026-10-14 — "you can even find an
// interchange and just look at it"; JD designs from seeing).
//
// Loads a REAL wafer through the production path (createDimensions +
// Dimension::load at 24 chunks, so waferSetup places the demo circuit,
// the spawn garden AND the interchanges exactly as the game does),
// finds the first interchange, meshes the chunks around it with the
// real mesher, and renders four software-renderer views:
//   interchange_overview.png  high oblique (the biome_capture angle)
//   interchange_street.png    ground level: from a building strip to the lamp
//   interchange_top.png       near-top-down: pad, plaza, paths, ring
//   interchange_reverse.png   high oblique from the opposite corner
// Plus interchange_manifest.json — the site's population manifest
// (RC/DC/Netor slots as data; see interchange.h).
// Output: files in the directory given as argv[1] (default ./interchange-shots).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "litholattice/core/math.h"
#include "litholattice/core/threading.h"  // JobPool
#include "litholattice/mcp/mcp_senses.h"  // encodePngRgb
#include "litholattice/render/block_materials.h"
#include "litholattice/render/mesher.h"
#include "litholattice/render/software_renderer.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/dimension.h"
#include "lithokraft/content/blocks.h"      // blockRegistry()
#include "lithokraft/game/dimensions.h"     // createDimensions
#include "lithokraft/game/interchange.h"    // planInterchangesIn, manifest
#include "lithokraft/game/resource_manager.h"

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr int kWorldChunks = 24;  // 384x384 blocks: several sites, none in the reserve
constexpr int kImgW = 1280, kImgH = 720;

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const size_t n = std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  return n == data.size();
}
bool writeText(const std::string& path, const std::string& s) {
  return writeFile(path, std::vector<uint8_t>(s.begin(), s.end()));
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

litholattice::Mat4 translation(float x, float y, float z) {
  litholattice::Mat4 t = litholattice::Mat4::identity();
  t.m[12] = x;
  t.m[13] = y;
  t.m[14] = z;
  return t;
}

struct Mesh {
  std::vector<litholattice::PackedVertex> verts;
  float ox, oy, oz;
};

bool renderView(const std::vector<Mesh>& meshes, const std::vector<uint32_t>& texBuffer,
                uint32_t layerCount, litholattice::Vec3 eye, litholattice::Vec3 target,
                const std::string& path) {
  litholattice::SoftwareRenderer sw;
  sw.setPerPixelShading(true);
  sw.setTextures(texBuffer.data(), layerCount);
  sw.resize(kImgW, kImgH);
  sw.clear(0xFF94B3DB);  // wafer day sky
  litholattice::Vec3 fwd{target.x - eye.x, target.y - eye.y, target.z - eye.z};
  const float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
  fwd.x /= len;
  fwd.y /= len;
  fwd.z /= len;
  const litholattice::Mat4 vp = litholattice::mul(
      litholattice::perspective(float(70.0 * kDegToRad), float(kImgW) / float(kImgH), 0.1f, 2000.f),
      litholattice::lookDir(eye, fwd));
  for (const auto& m : meshes) {
    const litholattice::Mat4 mvp = litholattice::mul(vp, translation(m.ox, m.oy, m.oz));
    sw.renderMesh(m.verts.data(), m.verts.size(), mvp.m);
  }
  const auto& fb = sw.framebuffer();
  std::vector<uint8_t> rgb(size_t(kImgW) * size_t(kImgH) * 3);
  for (size_t i = 0, n = rgb.size() / 3; i < n; ++i) {
    const uint32_t p = fb.pixels[i];
    rgb[3 * i] = uint8_t((p >> 16) & 0xFF);
    rgb[3 * i + 1] = uint8_t((p >> 8) & 0xFF);
    rgb[3 * i + 2] = uint8_t(p & 0xFF);
  }
  std::vector<uint8_t> png;
  if (!litholattice::mcp::encodePngRgb(kImgW, kImgH, rgb.data(), png)) return false;
  if (!writeFile(path, png)) return false;
  std::printf("[OK] %s (%zu bytes)\n", path.c_str(), png.size());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string outDir = argc > 1 ? argv[1] : "./interchange-shots";
  std::filesystem::create_directories(outDir);

  // Production worldgen path (pack wafer.json when available, exactly
  // like biome_capture; the wafer seed pin lives in makeGenerator).
  litholattice::WorldgenSettings settings;
  settings.seed = 20261011;
  const std::string waferJsonText =
      readFile("/home/hatch/workspace/litholattice/game/LithoKraft/assets/worldgen/wafer.json");
  if (!waferJsonText.empty()) settings.generators["wafer"] = waferJsonText;
  litholattice::DimensionRegistry reg = lithokraft::createDimensions(settings);
  litholattice::Dimension* wafer = reg.find("wafer");
  if (!wafer || !wafer->generator) {
    std::fprintf(stderr, "interchange_capture: no wafer generator\n");
    return 1;
  }
  const litholattice::BlockRegistry& blocks = lithokraft::blockRegistry();
  litholattice::JobPool pool(4);
  wafer->load(pool, kWorldChunks);
  litholattice::ChunkMap& map = wafer->chunks;
  const int extent = kWorldChunks * 16;

  // First interchange whose footprint is fully inside the world (the
  // same filter placeInterchanges applied during waferSetup).
  using T = lithokraft::InterchangeTuning;
  lithokraft::InterchangePlan site;
  bool found = false;
  for (const auto& p : lithokraft::planInterchangesIn(0, 0, 0, extent, extent)) {
    if (p.centreX - T::kPadRadius - 4 >= 0 && p.centreZ - T::kPadRadius - 4 >= 0 &&
        p.centreX + T::kPadRadius + 4 < extent && p.centreZ + T::kPadRadius + 4 < extent) {
      site = p;
      found = true;
      break;
    }
  }
  if (!found) {
    std::fprintf(stderr, "interchange_capture: no interchange in a %d-chunk wafer\n", kWorldChunks);
    return 1;
  }
  // padY measured off the built plaza paving at (centre+5, centre):
  // nothing stands there, so surfaceY is exactly the pad top.
  const int padY = map.surfaceY(site.centreX + 5, site.centreZ);
  std::printf("interchange_capture: site region (%d, %d) centre (%d, %d) padY %d mix %s\n",
              site.regionX, site.regionZ, site.centreX, site.centreZ, padY,
              site.mix == lithokraft::InterchangeMix::ReceiptorHeavy ? "receiptor_heavy"
                                                                     : "deceitor_heavy");

  // Manifest (resolved population) next to the PNGs.
  lithokraft::BuiltInterchange built;
  built.plan = site;
  built.padY = padY;
  built.population = site.population;
  for (auto& s : built.population) s.y = padY + s.y;
  if (!writeText(outDir + "/interchange_manifest.json", lithokraft::interchangeManifestJson(built)))
    std::fprintf(stderr, "interchange_capture: manifest write failed\n");
  else
    std::printf("[OK] %s/interchange_manifest.json\n", outDir.c_str());

  // Real textures, same loading as biome_capture.
  const std::string engineAssets = "/home/hatch/workspace/litholattice/engine/assets";
  const std::string gameAssets = "/home/hatch/workspace/litholattice/game/LithoKraft/assets";
  lithokraft::ResourceManager rm({engineAssets, gameAssets,
                                  std::filesystem::path(gameAssets) / "packs" / "pack_64"});
  rm.reload();
  const auto materialsPtr = rm.materials();
  litholattice::MeshContext mctx{blocks, *materialsPtr, nullptr};
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
  std::printf("interchange_capture: %u texture layers loaded\n", layerCount);

  // Mesh the chunks around the site (7x7 chunks covers pad + setting).
  std::vector<Mesh> meshes;
  const int ccx = site.centreX >> 4, ccz = site.centreZ >> 4;
  for (int dx = -3; dx <= 3; ++dx)
    for (int dz = -3; dz <= 3; ++dz)
      for (int sy = 0; sy < litholattice::kSubChunks; ++sy) {
        auto in = litholattice::snapshotForMeshing(map, blocks, ccx + dx, sy, ccz + dz, 0, 0);
        if (!in) continue;
        litholattice::ChunkMesh m = litholattice::meshSubChunk(*in, mctx);
        if (m.verts.empty()) continue;
        meshes.push_back(Mesh{std::move(m.verts), float((ccx + dx) * 16), float(sy * 16),
                              float((ccz + dz) * 16)});
      }
  std::printf("interchange_capture: meshed %zu sub-chunks\n", meshes.size());
  if (meshes.empty()) {
    std::fprintf(stderr, "interchange_capture: no meshes\n");
    return 1;
  }

  const float cx = float(site.centreX), cz = float(site.centreZ), py = float(padY);
  int failures = 0;
  const litholattice::Vec3 lampTop{cx, py + 4.f, cz};
  // 1) Establishing oblique (biome_capture's angle).
  if (!renderView(meshes, texBuffer, layerCount, {cx + 46.f, py + 34.f, cz + 46.f},
                  {cx, py + 1.f, cz}, outDir + "/interchange_overview.png"))
    ++failures;
  // 2) Street level: stand on a building strip, look across the plaza.
  if (!renderView(meshes, texBuffer, layerCount, {cx + 19.f, py + 3.2f, cz - 7.f}, lampTop,
                  outDir + "/interchange_street.png"))
    ++failures;
  // 3) Near-top-down: pad, plaza, paths, ring layout.
  if (!renderView(meshes, texBuffer, layerCount, {cx + 9.f, py + 72.f, cz + 9.f},
                  {cx, py, cz}, outDir + "/interchange_top.png"))
    ++failures;
  // 4) Reverse oblique.
  if (!renderView(meshes, texBuffer, layerCount, {cx - 48.f, py + 28.f, cz - 48.f},
                  {cx, py + 1.f, cz}, outDir + "/interchange_reverse.png"))
    ++failures;

  std::printf("interchange_capture: %d failures\n", failures);
  return failures ? 1 : 0;
}
