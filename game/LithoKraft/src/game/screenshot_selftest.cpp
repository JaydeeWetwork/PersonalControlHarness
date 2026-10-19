// Screenshot self-test: command registration/parsing, the capture pipeline
// (PNG validity, dimensions, lowres agent fidelity) against a fake
// framebuffer grabber, and the Fragment gallery round-trip. Headless: no GL
// context, no window. LF line endings.
#include "lithokraft/game/screenshot_selftest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "litholattice/console/concmd.h"
#include "lithokraft/game/fragment_gallery.h"
#include "lithokraft/game/game_commands.h"
#include "lithokraft/game/screenshot.h"
#include "litholattice/render/renderer.h"
#include "litholattice/render/texture_array.h"

namespace lithokraft {
namespace {

int fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}

// -- Fake framebuffer grabber ------------------------------------------------
// Implements IRenderer with a synthetic RGB frame; no GPU involved.
class FakeGrabber : public litholattice::IRenderer {
 public:
  int gw = 0, gh = 0;
  std::vector<uint8_t> pixels;  ///< top-first RGB, gw*gh*3
  bool failGrab = false;

  const char* name() const override { return "fake-grabber"; }
  std::string deviceInfo() const override { return "fake"; }
  litholattice::ITextureArray& textureArray() override { return ta_; }
  void setMaterials(const litholattice::BlockMaterials&) override {}
  int loadShaders(const litholattice::ShaderLibrary&) override { return 0; }
  void uploadChunkMesh(const litholattice::ChunkMesh&) override {}
  void uploadEntityMesh(const std::vector<litholattice::PackedVertex>&) override {}
  void clearChunkMeshes() override {}
  void renderFrame(const litholattice::FrameParams&) override {}
  litholattice::RenderStats stats() const override { return {}; }
  void requestGrab() override { armed_ = true; }
  bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) override {
    if (failGrab || !armed_) return false;
    armed_ = false;
    w = gw;
    h = gh;
    rgb = pixels;
    return true;
  }

 private:
  struct FakeTA : litholattice::ITextureArray {
    bool upload(const litholattice::TextureRegistry&) override { return true; }
    int layerCount() const override { return 0; }
    int tileSize() const override { return 16; }
  } ta_;
  bool armed_ = false;
};

void fillGradient(FakeGrabber& g, int w, int h) {
  g.gw = w;
  g.gh = h;
  g.pixels.resize(size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = g.pixels.data() + (size_t(y) * w + x) * 3;
      p[0] = uint8_t(x * 255 / (w - 1));
      p[1] = uint8_t(y * 255 / (h - 1));
      p[2] = uint8_t((x + y) * 255 / (w + h - 2));
    }
}

void fillSolid(FakeGrabber& g, int w, int h, uint8_t r, uint8_t gg,
               uint8_t b) {
  g.gw = w;
  g.gh = h;
  g.pixels.assign(size_t(w) * h * 3, 0);
  for (size_t i = 0; i < g.pixels.size(); i += 3) {
    g.pixels[i] = r;
    g.pixels[i + 1] = gg;
    g.pixels[i + 2] = b;
  }
}

// Reads the IHDR dimensions out of raw PNG bytes (signature + len + "IHDR").
bool pngDims(const std::vector<uint8_t>& png, int& w, int& h) {
  if (png.size() < 24) return false;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  for (int i = 0; i < 8; ++i)
    if (png[i] != sig[i]) return false;
  if (png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R')
    return false;
  w = (png[16] << 24) | (png[17] << 16) | (png[18] << 8) | png[19];
  h = (png[20] << 24) | (png[21] << 16) | (png[22] << 8) | png[23];
  return true;
}

// -- Command registration / parsing ------------------------------------------
void testCommandParsing() {
  using litholattice::console::ConsoleRegistry;
  ConsoleRegistry r;
  std::vector<ScreenshotRequest> seen;
  std::vector<std::string> printed;
  r.setOutput([&](const std::string& line) { printed.push_back(line); });
  GameCommandBindings b;
  b.screenshot = [&](const ScreenshotRequest& req) {
    seen.push_back(req);
    return std::string("ok");
  };
  registerGameCommands(r, std::move(b));

  auto run = [&](const char* line) {
    seen.clear();
    printed.clear();
    r.executeLine(line);
  };
  auto printedHas = [&](const char* sub) {
    for (const auto& l : printed)
      if (l.find(sub) != std::string::npos) return true;
    return false;
  };

  run("screenshot");
  check(seen.size() == 1 && seen[0].path.empty() && !seen[0].lowres &&
            !seen[0].toFragment && printedHas("ok"),
        "screenshot: bare call -> empty path (the game auto-names), binding called");
  run("screenshot lowres");
  check(seen.size() == 1 && seen[0].lowres && seen[0].path.empty(),
        "screenshot: 'lowres' flag parsed, still auto-named");
  run("screenshot \"my shot.png\" lowres");
  check(seen.size() == 1 && seen[0].path == "my shot.png" && seen[0].lowres,
        "screenshot: quoted path + lowres parsed");
  run("screenshot out.png");
  check(seen.size() == 1 && seen[0].path == "out.png" && !seen[0].lowres,
        "screenshot: plain path parsed");
  run("screenshot out.png bogus");
  check(seen.empty() && printedHas("usage:"),
        "screenshot: bad second flag prints usage, binding not called");
  run("screenshot fragment \"wafer biome at dusk\"");
  check(seen.size() == 1 && seen[0].toFragment &&
            seen[0].caption == "wafer biome at dusk" && !seen[0].lowres,
        "screenshot: fragment mode with quoted caption");
  run("screenshot fragment lowres quick snap");
  check(seen.size() == 1 && seen[0].toFragment && seen[0].lowres &&
            seen[0].caption == "quick snap",
        "screenshot: fragment lowres + multi-word caption");
  run("screenshot fragment");
  check(seen.size() == 1 && seen[0].toFragment && seen[0].caption.empty(),
        "screenshot: fragment without caption");
  run("screenshot a b c");
  check(seen.empty() && printedHas("usage:"),
        "screenshot: too many args prints usage, binding not called");
}

// -- Capture pipeline ---------------------------------------------------------
void testCapturePipeline() {
  FakeGrabber g;
  CapturedShot shot;
  std::string err;

  fillGradient(g, 96, 64);
  g.requestGrab();
  err.clear();
  bool ok = captureScreenshot(g, false, 160, 90, shot, &err);
  int pw = 0, ph = 0;
  check(ok && err.empty() && pngDims(shot.png, pw, ph) && pw == 96 && ph == 64 &&
            shot.width == 96 && shot.height == 64,
        "capture: PNG signature + IHDR valid, 96x64 dims match the framebuffer");

  fillGradient(g, 320, 200);
  g.requestGrab();
  ok = captureScreenshot(g, false, 160, 90, shot, &err);
  check(ok && shot.width == 320 && shot.height == 200 &&
            pngDims(shot.png, pw, ph) && pw == 320 && ph == 200,
        "capture: 320x200 framebuffer -> 320x200 PNG (full fidelity)");

  // Lowres: agent-vision fidelity (mcp.vision_width x mcp.vision_height).
  fillGradient(g, 640, 480);
  g.requestGrab();
  ok = captureScreenshot(g, true, 160, 90, shot, &err);
  check(ok && shot.width == 160 && shot.height == 90 &&
            pngDims(shot.png, pw, ph) && pw == 160 && ph == 90,
        "capture: lowres 640x480 -> 160x90 PNG (vision config dims)");

  // Box average of a solid color is the color itself.
  fillSolid(g, 64, 64, 200, 100, 50);
  std::vector<uint8_t> small;
  downscaleBoxAverage(g.pixels.data(), 64, 64, small, 16, 16);
  bool solid = small.size() == 16 * 16 * 3;
  for (size_t i = 0; solid && i < small.size(); i += 3)
    solid = small[i] == 200 && small[i + 1] == 100 && small[i + 2] == 50;
  check(solid, "downscale: solid 64x64 -> 16x16 stays exactly the color");

  // 2x2 checker (black, white / white, black) -> 1x1 ~mid gray.
  {
    const uint8_t checker[12] = {0, 0, 0, 255, 255, 255,
                                 255, 255, 255, 0, 0, 0};
    std::vector<uint8_t> one;
    downscaleBoxAverage(checker, 2, 2, one, 1, 1);
    check(one.size() == 3 && one[0] >= 126 && one[0] <= 129 &&
              one[0] == one[1] && one[1] == one[2],
          "downscale: 2x2 checker -> 1x1 mid gray (area average)");
  }

  // Grab failure propagates as an error, not a crash.
  FakeGrabber bad;
  bad.failGrab = true;
  bad.requestGrab();
  err.clear();
  check(!captureScreenshot(bad, false, 160, 90, shot, &err) && !err.empty(),
        "capture: backend grab failure -> false + message");
  // Unarmed grab (no requestGrab) also fails cleanly.
  FakeGrabber unarmed;
  fillGradient(unarmed, 32, 32);
  err.clear();
  check(!captureScreenshot(unarmed, false, 160, 90, shot, &err) &&
            !err.empty(),
        "capture: unarmed grab -> false + message");
}

// -- File helpers --------------------------------------------------------------
void testFileHelpers() {
  const std::string p1 = autoScreenshotPath(false);
  const std::string p2 = autoScreenshotPath(true);
  check(p1.rfind("screenshots/lithokraft-", 0) == 0 &&
            p1.size() > 30 && p1.compare(p1.size() - 4, 4, ".png") == 0,
        "autoScreenshotPath: screenshots/lithokraft-<timestamp>.png");
  check(p2.rfind("screenshots/lithokraft-", 0) == 0 &&
            p2.compare(p2.size() - 11, 11, "-lowres.png") == 0,
        "autoScreenshotPath: lowres variant ends -lowres.png");

  namespace fs = std::filesystem;
  const fs::path dir =
      fs::temp_directory_path() / "lithokraft-shot-selftest" / "deep" / "shot.png";
  const std::vector<uint8_t> bytes{1, 2, 3, 4, 5};
  std::string err;
  bool ok = writeFileBytes(dir.string(), bytes, &err);
  std::vector<uint8_t> back;
  if (ok) {
    std::ifstream in(dir, std::ios::binary);
    back.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
  }
  check(ok && err.empty() && back == bytes,
        "writeFileBytes: creates parent dirs, round-trips bytes");
  std::error_code ec;
  fs::remove_all(fs::temp_directory_path() / "lithokraft-shot-selftest", ec);
}

// -- Fragment gallery ------------------------------------------------------------
void testFragmentGallery() {
  FragmentGallery g;
  FragmentImage a;
  a.caption = "wafer biome at dusk";
  a.width = 160;
  a.height = 90;
  a.tick = 12345;
  a.x = 1.5;
  a.y = 64.0;
  a.z = -3.25;
  a.dimension = "wafer";
  a.png = {137, 80, 78, 71, 1, 2, 3};
  FragmentImage b;
  b.id = "custom-id";
  b.caption = "second";
  b.width = 320;
  b.height = 200;
  b.png = {9, 9, 9};
  const std::string idA = g.add(a);
  const std::string idB = g.add(b);
  check(!idA.empty() && idB == "custom-id" && idA != idB && g.size() == 2 &&
            g.ids()[0] == idA && g.ids()[1] == idB,
        "gallery: add assigns unique ids, keeps insertion order");
  const FragmentImage* got = g.get(idA);
  check(got && got->caption == "wafer biome at dusk" && got->tick == 12345 &&
            got->x == 1.5 && got->z == -3.25 && got->dimension == "wafer" &&
            got->png == a.png && g.get("nope") == nullptr,
        "gallery: get returns fields; unknown id -> nullptr");

  const std::vector<uint8_t> blob = g.serialize();
  FragmentGallery g2;
  std::string err;
  bool ok = g2.deserialize(blob.data(), blob.size(), &err);
  const FragmentImage* g2a = g2.get(idA);
  check(ok && err.empty() && g2.size() == 2 && g2a &&
            g2a->caption == "wafer biome at dusk" && g2a->width == 160 &&
            g2a->height == 90 && g2a->tick == 12345 && g2a->x == 1.5 &&
            g2a->y == 64.0 && g2a->z == -3.25 && g2a->dimension == "wafer" &&
            g2a->png == a.png && g2.ids() == g.ids(),
        "gallery: serialize/deserialize round-trips everything, order kept");
  check(g2.remove(idA) && g2.size() == 1 && !g2.remove(idA) &&
            g2.get(idA) == nullptr,
        "gallery: remove drops the entry; unknown remove -> false");
  g2.clear();
  check(g2.size() == 0 && g2.ids().empty(),
        "gallery: clear empties it");
  const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  check(!g2.deserialize(junk, sizeof junk, &err) && !err.empty() &&
            g2.size() == 0,
        "gallery: garbage blob fails cleanly, gallery unchanged");
  std::vector<uint8_t> truncated = blob;
  truncated.resize(truncated.size() / 2);
  check(!g2.deserialize(truncated.data(), truncated.size(), &err) &&
            !err.empty() && g2.size() == 0,
        "gallery: truncated blob fails cleanly, gallery unchanged");
  truncated = blob;
  truncated.push_back(0xFF);  // trailing byte
  check(!g2.deserialize(truncated.data(), truncated.size(), &err) &&
            g2.size() == 0,
        "gallery: trailing bytes fail cleanly, gallery unchanged");
}

// -- Session tier (P1b, App. F M6): quit/save/map refused for restricted masks --
void testSessionTier() {
  using litholattice::console::ConsoleRegistry;
  using litholattice::console::kCallerFullAuthority;
  ConsoleRegistry r;
  std::vector<std::string> printed;
  r.setOutput([&](const std::string& line) { printed.push_back(line); });
  int quits = 0, saves = 0, maps = 0;
  GameCommandBindings b;
  b.quit = [&] { ++quits; };
  b.save = [&] { ++saves; return std::string("saved"); };
  b.map = [&](const std::string&) { ++maps; return std::string("mapped"); };
  b.dimensionIds = [] { return std::vector<std::string>{"wafer"}; };
  registerGameCommands(r, std::move(b));
  auto printedHas = [&](const char* sub) {
    for (const auto& l : printed)
      if (l.find(sub) != std::string::npos) return true;
    return false;
  };

  // Restricted caller (MCP agent / mod mask): all three refused, bindings
  // never invoked, even though none of them is cheat-flagged.
  r.setCallerPermissions(0);
  printed.clear();
  r.executeLine("quit");
  r.executeLine("save");
  r.executeLine("map wafer");
  check(quits == 0 && saves == 0 && maps == 0 &&
            printedHas("command 'quit' requires session authority") &&
            printedHas("command 'save' requires session authority") &&
            printedHas("command 'map' requires session authority"),
        "session tier: restricted mask refused quit/save/map, bindings not called");

  // Full-authority local player: all three still work.
  r.setCallerPermissions(kCallerFullAuthority);
  printed.clear();
  r.executeLine("quit");
  r.executeLine("save");
  r.executeLine("map wafer");
  check(quits == 1 && saves == 1 && maps == 1,
        "session tier: full-authority local player keeps quit/save/map");
}

// -- Screenshot path confinement (P1b, App. F M6) -------------------------------
void testPathConfinement() {
  namespace fs = std::filesystem;
  const fs::path root =
      fs::temp_directory_path() / "lithokraft-shot-selftest-root" / "screenshots";
  auto insideRoot = [&](const fs::path& p) {
    auto ri = root.begin();
    auto pi = p.begin();
    for (; ri != root.end(); ++ri, ++pi)
      if (pi == p.end() || *pi != *ri) return false;
    return pi != p.end();
  };

  // In-root relative paths resolve under the root (subdirs allowed).
  const auto plain = resolveScreenshotPath(root, "out.png", false);
  check(!plain.fellBack && plain.path == root / "out.png",
        "screenshot confinement: plain filename lands in the screenshots root");
  const auto nested = resolveScreenshotPath(root, "trips/day1.png", false);
  check(!nested.fellBack && nested.path == root / "trips" / "day1.png",
        "screenshot confinement: in-root subdirectory path works");

  // Escapes are refused: the result falls back to the confined auto-name
  // inside the root — never the escaped destination.
  const auto dotdot = resolveScreenshotPath(root, "../evil.png", false);
  check(dotdot.fellBack && insideRoot(dotdot.path) &&
            dotdot.path.filename().string().rfind("lithokraft-", 0) == 0,
        "screenshot confinement: '..' escape refused -> confined auto-name in root");
  const auto deep = resolveScreenshotPath(root, "sub/../../evil.png", false);
  check(deep.fellBack && insideRoot(deep.path),
        "screenshot confinement: nested '..' escape refused -> inside root");
  const auto absolute = resolveScreenshotPath(root, "/tmp/evil.png", false);
  check(absolute.fellBack && insideRoot(absolute.path),
        "screenshot confinement: absolute path refused -> confined auto-name in root");

  // Missing arg: confined auto-name, not a fallback.
  const auto autoname = resolveScreenshotPath(root, "", true);
  check(!autoname.fellBack && insideRoot(autoname.path) &&
            autoname.path.filename().string().find("-lowres.png") != std::string::npos,
        "screenshot confinement: empty path -> auto-name in root (lowres suffix kept)");

  // And an in-root write through the confined path actually round-trips.
  const std::vector<uint8_t> bytes{9, 8, 7};
  std::string err;
  const bool ok = writeFileBytes(plain.path.string(), bytes, &err);
  check(ok && fs::exists(root / "out.png"),
        "screenshot confinement: confined path is writable via writeFileBytes");
  std::error_code ec;
  fs::remove_all(root.parent_path(), ec);
}

}  // namespace

int runScreenshotSelfTest() {
  fails = 0;
  std::printf("screenshot selftest:\n");
  testCommandParsing();
  testSessionTier();
  testPathConfinement();
  testCapturePipeline();
  testFileHelpers();
  testFragmentGallery();
  std::printf("%s\n", fails ? "SCREENSHOT SELFTEST: FAILED" : "SCREENSHOT SELFTEST: OK");
  return fails;
}

}  // namespace lithokraft
