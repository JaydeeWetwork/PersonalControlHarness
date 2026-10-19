// LithoKraft: application entry. Parses options and wires the modules together:
// platform (window + GLFW input backend) -> input (actions) -> render backend (software, the
// default: the CPU baseline every machine runs; gl33, or Vulkan with --renderer vulkan,
// falling back to gl33) -> game.
// Input runs on the main thread only (see docs/ARCHITECTURE.md, "Input").
#include <atomic>
#include <charconv>
#include <mutex>
#include <system_error>
#if !defined(_WIN32)
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cstdio>
#include "litholattice/core/process_memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "litholattice/assets/mod_manifest.h"
#include "litholattice/assets/mod_signature.h"
#include "litholattice/procgen/struct_registry.h"
#include "litholattice/assets/image.h"
#include "noise_preview.h"
#include "scripts_dir.h"
#include "selftest.h"
#include "perf_headless.h"
#include "handshake_test.h"
#include "control_latency.h"
#include "control_stream_test.h"
#include "demo_record_test.h"
#include "litholattice/audio/audio_engine.h"
#include "litholattice/audio/audio_output.h"
#include "litholattice/core/log.h"
#include "litholattice/core/threading.h"
#include "litholattice/core/time.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/game/actions.h"
#include "lithokraft/game/game.h"
#include "lithokraft/game/world_save.h"  // SV-H2: WorldSave::readWorldSeed (level.json) before Game construction
#include "lithokraft/game/resource_manager.h"
#include "lithokraft/game/settings.h"
#include "litholattice/input/input_system.h"
#include "litholattice/platform/window.h"
#include "litholattice/render/pbr_materials.h"
#include "litholattice/render/renderer.h"
#include "litholattice/mcp/server.h"  // McpServer + StdioTransport (--mcp-stdio, P2c F-M9)
#include "litholattice/render/frame_pacer.h"  // FramePacer (render.fps_cap drive, D-M3)
#include "litholattice/render/software_presenter.h"
#include "litholattice/render/software_renderer.h"
#include "litholattice/render_vk/vulkan_renderer.h"

#ifndef LITHOKRAFT_SOURCE_DIR
#define LITHOKRAFT_SOURCE_DIR ""
#endif
#ifndef LITHOKRAFT_ENGINE_ASSETS_DIR
#define LITHOKRAFT_ENGINE_ASSETS_DIR ""
#endif
#ifndef LITHOKRAFT_VERSION
#define LITHOKRAFT_VERSION "dev"
#endif

using namespace lithokraft;

#if !defined(_WIN32)
// MCP stdio transport for --mcp-stdio (P2c, F-M9): raw-fd, poll-based.
// Two properties the engine's FdStdioTransport does not have, both needed
// for a clean in-game wiring:
//  - it writes to a SAVED copy of the real stdout fd, so main() can point
//    fd 1 at stderr for the process's log output (logInfo writes to
//    stdout and would corrupt the JSON-RPC stream) while MCP responses
//    still reach the real stdout, byte-clean;
//  - readLine() polls with a timeout and honours requestStop(), so the
//    game can shut the server thread down deterministically on exit
//    (join, never detach) instead of stranding it in a blocking read.
class McpStdioTransport final : public litholattice::mcp::StdioTransport {
 public:
  explicit McpStdioTransport(int outFd) : outFd_(outFd) {}
  void requestStop() { stop_.store(true, std::memory_order_relaxed); }

  bool readLine(std::string& out) override {
    for (;;) {
      if (const size_t nl = buffer_.find('\n'); nl != std::string::npos) {
        out = buffer_.substr(0, nl);
        buffer_.erase(0, nl + 1);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      if (stop_.load(std::memory_order_relaxed)) return false;
      pollfd pfd{};
      pfd.fd = STDIN_FILENO;
      pfd.events = POLLIN;
      const int r = ::poll(&pfd, 1, 100);
      if (r < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (r == 0) continue;  // timeout: re-check the stop flag
      char chunk[8192];
      const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof chunk);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) {  // EOF: a trailing unterminated line still counts
        if (buffer_.empty()) return false;
        out = std::move(buffer_);
        buffer_.clear();
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      buffer_.append(chunk, size_t(n));
    }
  }

  void writeLine(std::string_view line) override {
    std::lock_guard<std::mutex> lk(writeMutex_);
    std::string out(line);
    out.push_back('\n');
    const char* p = out.data();
    size_t left = out.size();
    while (left > 0) {
      const ssize_t n = ::write(outFd_, p, left);
      if (n < 0) {
        if (errno == EINTR) continue;
        return;  // peer gone: drop the line, the read side will see EOF/stop
      }
      p += n;
      left -= size_t(n);
    }
  }

 private:
  int outFd_;
  std::atomic<bool> stop_{false};
  std::string buffer_;
  std::mutex writeMutex_;
};
#endif  // !_WIN32

namespace lithokraft {

/// The window's OS clipboard for the UI.
class WindowClipboard final : public ui::IClipboard {
 public:
  explicit WindowClipboard(Window& w) : window_(w) {}
  std::string text() const override { return window_.clipboardText(); }
  void setText(const std::string& text) override { window_.setClipboardText(text); }

 private:
  Window& window_;
};

// Pack directories: the engine base pack (assets/: pack.json + shaders/) and the game pack
// (LithoKraft/assets/: ui/, worldgen/, art packs in packs/). Looked up next to the exe (shipped
// layout: <exe dir>/assets and <exe dir>/LithoKraft/assets), else in the source tree. Empty if not found.
struct AssetDirs {
  std::string engine, game;
};

AssetDirs findAssetDirs(const char* argv0) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path exeDir = fs::absolute(fs::path(argv0), ec).parent_path();
  const auto first = [&ec](std::initializer_list<fs::path> candidates) -> std::string {
    for (const fs::path& a : candidates)
      if (fs::is_regular_file(a / "pack.json", ec)) return a.lexically_normal().string();
    return {};
  };
  return {first({exeDir / "assets", fs::path(LITHOKRAFT_ENGINE_ASSETS_DIR)}),
          first({exeDir / "LithoKraft" / "assets", fs::path(LITHOKRAFT_SOURCE_DIR) / "assets"})};
}

// mod.json: validate the game's own manifest and list <exe>/mods. Content mods are accepted
// silently (decision Q6) and returned for the pack stack; script / native mods wait for the
// permission prompt (not built yet).
std::vector<std::filesystem::path> scanMods(const char* argv0) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path exeDir = fs::absolute(fs::path(argv0), ec).parent_path();
  const ModManifest engine = *ModManifest::parse(R"({"id":"litholattice","version":"0.1.0"})", nullptr);
  bool gameSealed = false;  // folded into the seal decision below
  for (const fs::path& p : {exeDir / "mod.json", fs::path(LITHOKRAFT_SOURCE_DIR) / "mod.json"}) {
    std::ifstream in(p, std::ios::binary);
    if (!in) continue;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    if (const auto game = ModManifest::parse(ss.str(), &err)) {
      logInfo("mods: game '%s' %s", game->id.c_str(), game->version.str().c_str());
      gameSealed = game->sealed;
    } else logError("mods: %s: %s", p.string().c_str(), err.c_str());
    break;
  }
  // Fail-closed key hygiene: the throwaway test key must never ship. In release
  // builds this is a hard boot refusal; in debug it's a loud warning (dev loop).
  // Runs before the mods/ early-return below so a stock install can't dodge it.
  if (litholattice::assets::bakedKeyIsTestKey()) {
#ifdef NDEBUG
    // Genuine boot refusal (not just "no mods"): returning an empty pack list
    // would let the game boot normally, defeating the fail-closed intent.
    logError("FATAL: baked-in public key is the retired TEST key — refusing to boot a release build. "
             "Install JD's real key (see mod_signature.h) and rebuild.");
    std::exit(1);
#else
    logError("WARNING: baked-in public key is the retired TEST key — mods will verify against it. "
             "Replace before beta (see mod_signature.h).");
#endif
  }
  // Trust anchor for structure signatures: JD's baked-in public key. Must be set before
  // any structure registers (mods load below). Set even when mods/ is missing so
  // JD-signed structures can verify Trusted on a stock install. Until set, no
  // structure can verify Trusted.
  litholattice::procgen::StructureRegistry::instance().setTrustedPublicKeyHex(
      litholattice::assets::kBakedPublicKeyHex);
  // Note: no mods/ directory is NOT an early return — the seal decision below must
  // still run (release with zero mods seals, fail closed). An empty ModSet falls
  // through to the same decision logic.
  const bool haveModsDir = fs::is_directory(exeDir / "mods", ec);
  if (!haveModsDir) logInfo("mods: no mods/ directory — continuing with zero mods (seal decision still applies)");
  // Containment (P1b, Appendix E M1, layer 2): loadModDirectory already
  // skips a single hostile mod with an error entry, but the boot boundary
  // catches anything that still escapes — a crafted mod folder must never
  // abort boot; on a catastrophic scan failure we continue with zero mods
  // (and the seal decision below still applies, fail-closed).
  ModSet set;
  if (haveModsDir) {
    try {
      set = loadModDirectory(exeDir / "mods", {engine});
    } catch (const std::exception& ex) {
      logError("mods: mod scan threw (%s) — continuing with zero mods", ex.what());
    } catch (...) {
      logError("mods: mod scan threw an unknown exception — continuing with zero mods");
    }
  }
  for (const std::string& e : set.errors) logError("mods: %s", e.c_str());
  // Boot seal decision (certificate trust model, JD 2026-10-12):
  //   - sealed:true in any manifest -> seal (sealed wins, unchanged).
  //   - Release: open ONLY when every loaded mod is Verified and at least one
  //     verified mod opts into hot_swap (the live-ops case). Any unsigned mod
  //     (or no verified hot-swap mod) -> seal (fail closed).
  //   - Debug: open unless sealed (developer path, unchanged).
  // Sealing is a side effect on the global registry (per-mod scoping is future work).
  {
    const ModManifest* sealedBy = nullptr;
    bool allVerified = true;
    bool anyVerifiedHotSwap = false;
    for (const ModManifest& m : set.ordered) {
      if (m.sealed && !sealedBy) sealedBy = &m;
      if (m.signatureStatus != SignatureStatus::Verified) allVerified = false;
      if (m.hotSwap && m.signatureStatus == SignatureStatus::Verified) anyVerifiedHotSwap = true;
      // Trust-tier log line: unsigned mods run sandboxed, clearly labeled.
      if (m.signatureStatus == SignatureStatus::Unsigned)
        logInfo("mods: '%s' is unsigned - sandboxed", m.id.c_str());
      else if (m.signatureStatus == SignatureStatus::Verified)
        logInfo("mods: '%s' signature verified", m.id.c_str());
    }
    bool seal = false;
    const char* reason = "";
    if (sealedBy != nullptr) {
      seal = true;
      reason = "sealed:true manifest";
    } else if (gameSealed) {
      seal = true;
      reason = "sealed:true in game manifest";
    }
#ifdef NDEBUG
    else if (allVerified && anyVerifiedHotSwap) {
      seal = false;
      reason = "release with verified hot-swap mod (live-ops)";
    } else {
      seal = true;
      reason = "release build (fail closed: no verified hot-swap mod)";
    }
#else
    else {
      seal = false;
      reason = "debug build (developer path)";
    }
#endif
    if (seal) {
      mutableBlockRegistry().content().seal();
      litholattice::procgen::StructureRegistry::instance().seal();
      logInfo("mods: registry sealed (%s)", reason);
    } else {
      logInfo("mods: registry open for hot-swap (%s)", reason);
    }
  }
  for (const ModManifest& m : set.ordered)
    if (m.type != ModType::Content) logInfo("mods: '%s' is a %s mod; waiting for the permission prompt (not loaded yet)", m.id.c_str(), m.type == ModType::Script ? "script" : "native");
  return contentModPacks(set);
}

// Window icon from LithoKraft/assets/icons (the exe's own icon comes from lithokraft.rc).
void setWindowIcon(Window& window, const AssetDirs& assets) {
  if (assets.game.empty()) return;
  std::vector<Image> images;
  for (const int size : {16, 24, 32, 48, 64, 256}) {
    std::string err;
    const auto file = std::filesystem::path(assets.game) / "icons" / ("lithokraft_icon_dark_" + std::to_string(size) + ".png");
    if (auto img = Image::load(file, &err)) images.push_back(std::move(*img));
  }
  std::vector<IconPixels> px;
  for (const Image& i : images) px.push_back({i.width(), i.height(), i.pixels().data()});
  window.setIcon(px.data(), int(px.size()));
  if (px.empty()) logError("window icon: no LithoKraft/assets/icons/lithokraft_icon_dark_*.png found");
}

// Pack stack, lowest first: engine base, game pack, the art pack, content mods (dependency
// order), then --resource-pack folders in order.
std::vector<std::filesystem::path> packStack(const AssetDirs& assets, const std::string& pack,
                                             const std::vector<std::string>& extra,
                                             const std::vector<std::filesystem::path>& mods = {}) {
  std::vector<std::filesystem::path> dirs;
  if (!assets.engine.empty()) dirs.emplace_back(assets.engine);
  if (!assets.game.empty()) {
    dirs.emplace_back(assets.game);
    dirs.emplace_back(std::filesystem::path(assets.game) / "packs" / pack);
  }
  for (const auto& m : mods) dirs.push_back(m);
  for (const auto& e : extra) dirs.emplace_back(e);
  return dirs;
}

// Writable data root: the source tree when it exists (logs/, config/), else the exe's folder.
std::filesystem::path dataRoot(const char* argv0) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path root = LITHOKRAFT_SOURCE_DIR;
  if (root.empty() || !fs::is_directory(root / "src", ec)) root = fs::absolute(fs::path(argv0), ec).parent_path();
  return root;
}

// Live log: <root>/logs/lithokraft.log.
std::string defaultLogPath(const char* argv0) { return (dataRoot(argv0) / "logs" / "lithokraft.log").lexically_normal().string(); }

// Bindings from <root>/config/input.json (created from the defaults on first run).
ActionMap loadBindings(const char* argv0) {
  const std::filesystem::path path = (dataRoot(argv0) / "config" / "input.json").lexically_normal();
  BindingsFile f = loadOrCreateBindings(path, defaultActionMap());
  switch (f.status) {
    case BindingsFile::Status::Created: logInfo("input: wrote default bindings to %s", path.string().c_str()); break;
    case BindingsFile::Status::Loaded: logInfo("input: bindings from %s", path.string().c_str()); break;
    case BindingsFile::Status::Invalid:
      logError("input: %s (%s); using defaults, file left untouched", path.string().c_str(), f.message.c_str());
      break;
  }
  for (const auto& w : f.warnings) logInfo("  input.json: %s", w.c_str());
  for (const auto& [a, b] : f.map.conflicts())
    logInfo("  input.json: %s and %s share an input", std::string(f.map.actions().name(a)).c_str(),
            std::string(f.map.actions().name(b)).c_str());
  return std::move(f.map);
}

/// The window and the renderer drawing into it (destroyed renderer first).
struct Display {
  std::unique_ptr<Window> window;
  std::unique_ptr<IRenderer> renderer;
  /// Software backend only: blits the CPU framebuffer into the window's back
  /// buffer before swap(); `software` is the same object as `renderer` then.
  std::unique_ptr<SoftwarePresenter> presenter;
  SoftwareRenderer* software = nullptr;
};

// Window + renderer for `backend` ("software", "gl33" or "vulkan"). Vulkan falls back to
// OpenGL 3.3 with a log line when the loader, a device or any init step is missing (its window
// is recreated for GL). Software renders on the CPU into its framebuffer; the window is still
// created for OpenGL, whose context the presenter borrows purely as the blit mechanism — if
// that present path cannot be built, software falls back to gl33 the same loud way.
Display createDisplay(const std::string& backend, bool vsync, bool validation, const std::string& present, const DisplayDesc& disp,
                      const std::string& vkArena, bool glArena) {
  Display d;
  if (backend == "software") {
    d.window = Window::create({1280, 720, "LithoKraft", vsync, GraphicsApi::OpenGL33, disp});
    if (d.window) {
      auto sw = std::make_unique<SoftwareRenderer>();
      // The textured, diffuse-lit path (biome_capture does the same on its
      // instance); without it the renderer stays in the flat faceColor mode
      // the MCP vision path deliberately uses. Per-instance state: the MCP
      // adapter's own renderer is unaffected.
      sw->setPerPixelShading(true);
      d.software = sw.get();
      d.renderer = std::move(sw);
      d.presenter = createSoftwarePresenter(&Window::glProcAddress);
      if (!d.presenter) {
        logError("renderer: software present path unavailable; falling back to OpenGL 3.3");
        d.software = nullptr;
        d.renderer = createGL33Renderer(&Window::glProcAddress, glArena);
      }
    }
    return d;
  }
  if (backend == "vulkan") {
    d.window = Window::create({1280, 720, "LithoKraft", vsync, GraphicsApi::Vulkan, disp});
    std::string err = "no Vulkan window";
    if (d.window) {
      Window& w = *d.window;
      VulkanRendererDesc desc;
      desc.surface.instanceExtensions = w.vulkanInstanceExtensions();
      desc.surface.createSurface = [&w](void* instance) { return w.createVulkanSurface(instance); };
      desc.vsync = vsync;
      desc.presentMode = present;
      desc.vertexArena = vkArena;
      desc.validation = validation;
      d.renderer = createVulkanRenderer(desc, &err);
    }
    if (d.renderer) return d;
    logError("renderer: Vulkan unavailable (%s); falling back to OpenGL 3.3", err.c_str());
    d.window.reset();
  }
  d.window = Window::create({1280, 720, "LithoKraft", vsync, GraphicsApi::OpenGL33, disp});
  if (d.window) d.renderer = createGL33Renderer(&Window::glProcAddress, glArena);
  return d;
}
}  // namespace lithokraft

/// fopen for writing without MSVC's C4996 (fopen_s there).
inline std::FILE* openForWrite(const char* path) {
#if defined(_MSC_VER)
  std::FILE* f = nullptr;
  return fopen_s(&f, path, "w") == 0 ? f : nullptr;
#else
  return std::fopen(path, "w");
#endif
}

/// Bench recorder: frame times, the main-thread breakdown, sim tick ms, streaming and memory,
/// per second (log lines) and for the whole run (BENCH line + optional JSON).
struct BenchRun {
  std::vector<double> frame, input, update, upload, render, present, tick, uploadBytes;
  std::vector<double> secFrame;
  uint64_t memStart = 0, memPeak = 0, memEnd = 0;
  std::vector<double> minuteFrame, minuteRam;  // soak drift
  double minSum = 0; int minFrames = 0, minuteNo = 0;
  double secInput = 0, secUpdate = 0, secUpload = 0, secRender = 0, secPresent = 0, secBytes = 0; int secFrames = 0;
};

double avgOf(const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / double(v.size()); }

/// q-quantile (0..1) of `v` (sorted in place); 0 when empty.
double percentile(std::vector<double>& v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

int main(int argc, char** argv) {
  GameConfig cfg;
  double runSeconds = 0, benchSeconds = 0; bool vsync = false, noSave = false;
  bool tpsGiven = false, radiusGiven = false, displayGiven = false;  // CLI wins over settings.json
  DisplayDesc display0;
  std::string vkArena = "device";
  bool glArena = true;
  std::string saveDir, presentMode, benchPath = "orbit", benchJson;
  std::string pack = "pack_64";
  std::vector<std::string> extraPacks;
  bool mute = false;
  std::string backend = "software";  // JD's standard: the CPU path is the baseline default;
                                     // gl33/vulkan are the GPU offload, selected explicitly
#ifdef NDEBUG
  bool vkValidation = false;
#else
  bool vkValidation = true;  // debug builds: Khronos validation layer when installed
#endif
  std::string logPath = defaultLogPath(argv[0]);
  std::string scriptsMode;  // --scripts source|exe|<dir>; empty = this build's default
  NoisePreviewOptions preview;
  bool runPreview = false, runSelftest = false, runPerf = false;
  bool runHandshakeTest = false, hsGiven = false, seedGiven = false;
  bool mcpStdio = false;  // --mcp-stdio: engine MCP server on stdin/stdout (P2c, F-M9)
  bool runScenario = false; int scenarioTicks = 0, scenarioWorkers = 4, scenarioPhases = 4;
  bool wantControlLatency = false, wantControlTest = false, wantControlScenario = false;
  bool wantDemoTest = false, wantDemoDump = false, wantDemoVerify = false;
  std::string demoFile; bool demoTicksGiven = false; uint64_t demoTicksLo = 0, demoTicksHi = 0;
  int controlScenarioTicks = 0; std::string controlScenarioMode = "legacy";
  std::string scenarioMode = "legacy";
  std::string perfJson;
  bool badArg = false;
  bool tpsUnlock = false;  // debug: lifts the 512 TPS cap for benches
  for (int i = 1; i < argc; ++i) {
    // Numeric option value: the whole next argument must parse and lie in [lo, hi], else the
    // run stops with a message (a typo used to become 0 silently, and sizes were unbounded).
    auto num = [&](double lo, double hi) -> double {
      const char* flag = argv[i];
      if (i + 1 >= argc) { std::fprintf(stderr, "%s: missing value\n", flag); badArg = true; return lo; }
      const char* s = argv[++i];
      double v = 0;
      const auto [end, ec] = std::from_chars(s, s + std::strlen(s), v);
      if (ec != std::errc{} || *end != '\0' || !(v >= lo && v <= hi)) {
        std::fprintf(stderr, "%s: '%s' is not a number in [%g, %g]\n", flag, s, lo, hi);
        badArg = true;
        return lo;
      }
      return v;
    };
    if (!std::strcmp(argv[i], "--size")) cfg.worldChunks = std::max(4, int(num(0, 1024)));
    else if (!std::strcmp(argv[i], "--tps")) { cfg.tps = int(num(1, 8192)); tpsGiven = true; }
    else if (!std::strcmp(argv[i], "--tps-unlock")) tpsUnlock = true;
    else if (!std::strcmp(argv[i], "--scene-size")) cfg.sceneSize = int(num(16, 1024));
    else if (!std::strcmp(argv[i], "--scene-rows")) cfg.sceneRows = int(num(1, 64));
    else if (!std::strcmp(argv[i], "--pattern") && i + 1 < argc) { cfg.pattern = argv[++i]; if (cfg.pattern != "checkers" && cfg.pattern != "circles") { std::fprintf(stderr, "--pattern: checkers|circles\n"); badArg = true; } }
    else if (!std::strcmp(argv[i], "--screen-orient") && i + 1 < argc) {
      const std::string o = argv[++i];
      if (o != "flat" && o != "vertical") { std::fprintf(stderr, "--screen-orient: flat|vertical\n"); badArg = true; }
      cfg.screenVertical = o == "vertical";
    }
    else if (!std::strcmp(argv[i], "--radius")) { cfg.tickRadius = int(num(0, 512)); radiusGiven = true; }
    else if (!std::strcmp(argv[i], "--seconds")) runSeconds = num(0, 1e9);
    else if (!std::strcmp(argv[i], "--bench")) benchSeconds = num(1, 3600);
    else if (!std::strcmp(argv[i], "--vsync")) vsync = true;
    else if (!std::strcmp(argv[i], "--vk-arena") && i + 1 < argc) {
      vkArena = argv[++i];
      if (vkArena != "host" && vkArena != "device" && vkArena != "rebar") { std::fprintf(stderr, "--vk-arena: host|device|rebar\n"); badArg = true; }
    }
    else if (!std::strcmp(argv[i], "--gl-arena") && i + 1 < argc) {
      const std::string v = argv[++i];
      if (v != "on" && v != "off") { std::fprintf(stderr, "--gl-arena: on|off\n"); badArg = true; }
      glArena = v == "on";
    }
    else if (!std::strcmp(argv[i], "--display") && i + 1 < argc) {
      displayGiven = true;
      if (!parseDisplayMode(argv[++i], display0.mode)) { std::fprintf(stderr, "--display: windowed|maximized|borderless|fullscreen\n"); badArg = true; }
    }
    else if (!std::strcmp(argv[i], "--monitor")) display0.monitor = int(num(0, 15));
    else if (!std::strcmp(argv[i], "--refresh")) display0.refreshHz = int(num(1, 1000));
    else if (!std::strcmp(argv[i], "--resolution") && i + 1 < argc) {
      char* end = nullptr;
      const char* res = argv[++i];
      display0.width = int(std::strtol(res, &end, 10));
      display0.height = (end && (*end == 'x' || *end == 'X')) ? int(std::strtol(end + 1, &end, 10)) : 0;
      if ((end && *end) || display0.width < 320 || display0.height < 200 ||
          display0.width > 16384 || display0.height > 16384) {
        std::fprintf(stderr, "--resolution: WxH, e.g. 1920x1080\n"); badArg = true;
      }
    }
    else if (!std::strcmp(argv[i], "--present") && i + 1 < argc) {
      presentMode = argv[++i];
      if (presentMode != "fifo" && presentMode != "mailbox" && presentMode != "immediate") { std::fprintf(stderr, "--present: fifo|mailbox|immediate\n"); badArg = true; }
    }
    else if (!std::strcmp(argv[i], "--bench-path") && i + 1 < argc) {
      benchPath = argv[++i];
      if (benchPath != "orbit" && benchPath != "stream" && benchPath != "flyover") { std::fprintf(stderr, "--bench-path: orbit|stream|flyover\n"); badArg = true; }
    }
    else if (!std::strcmp(argv[i], "--soak")) {
      benchSeconds = 60.0 * num(1, 1440);
      benchPath = "soak";
    }
    else if (!std::strcmp(argv[i], "--bench-json") && i + 1 < argc) benchJson = argv[++i];
    else if (!std::strcmp(argv[i], "--no-save")) noSave = true;
    else if (!std::strcmp(argv[i], "--save-dir") && i + 1 < argc) saveDir = argv[++i];
    else if (!std::strcmp(argv[i], "--no-euv")) cfg.euv = false;
    else if (!std::strcmp(argv[i], "--timing-handshake")) { cfg.timingHandshake = true; hsGiven = true; }
    else if (!std::strcmp(argv[i], "--no-timing-handshake")) { cfg.timingHandshake = false; hsGiven = true; }
    else if (!std::strcmp(argv[i], "--handshake-phases")) cfg.handshakePhases = int(num(1, 16));
    else if (!std::strcmp(argv[i], "--handshake-test")) runHandshakeTest = true;
    else if (!std::strcmp(argv[i], "--control-latency")) wantControlLatency = true;
    else if (!std::strcmp(argv[i], "--control-test")) wantControlTest = true;
    else if (!std::strcmp(argv[i], "--demo-test")) wantDemoTest = true;
    else if (!std::strcmp(argv[i], "--demo-dump")) {
      if (i + 1 >= argc) { std::fprintf(stderr, "--demo-dump: needs FILE\n"); badArg = true; }
      else { wantDemoDump = true; demoFile = argv[++i]; }
    }
    else if (!std::strcmp(argv[i], "--demo-verify")) {
      if (i + 1 >= argc) { std::fprintf(stderr, "--demo-verify: needs FILE\n"); badArg = true; }
      else { wantDemoVerify = true; demoFile = argv[++i]; }
    }
    else if (!std::strcmp(argv[i], "--ticks")) {
      if (i + 2 >= argc) { std::fprintf(stderr, "--ticks: needs LO HI\n"); badArg = true; }
      else {
        demoTicksGiven = true;
        const char* loS = argv[++i];
        const char* hiS = argv[++i];
        const auto [loEnd, loEc] = std::from_chars(loS, loS + std::strlen(loS), demoTicksLo);
        const auto [hiEnd, hiEc] = std::from_chars(hiS, hiS + std::strlen(hiS), demoTicksHi);
        if (loEc != std::errc{} || *loEnd != '\0' || hiEc != std::errc{} || *hiEnd != '\0') {
          std::fprintf(stderr, "--ticks: LO and HI must be unsigned 64-bit numbers\n"); badArg = true;
        }
      }
    }
    else if (!std::strcmp(argv[i], "--control-scenario")) {
      wantControlScenario = true;
      if (i + 2 >= argc) { std::fprintf(stderr, "--control-scenario: needs TICKS legacy|handshake\n"); badArg = true; }
      else {
        controlScenarioTicks = int(num(1, 1000000));
        controlScenarioMode = argv[++i];
        if (controlScenarioMode != "legacy" && controlScenarioMode != "handshake" && controlScenarioMode != "baseline") { std::fprintf(stderr, "--control-scenario: mode must be legacy|handshake|baseline\n"); badArg = true; }
      }
    }
    else if (!std::strcmp(argv[i], "--mcp-stdio")) mcpStdio = true;
    else if (!std::strcmp(argv[i], "--handshake-scenario")) {
      runScenario = true;
      if (i + 4 >= argc) { std::fprintf(stderr, "--handshake-scenario: needs TICKS legacy|handshake WORKERS PHASES\n"); badArg = true; }
      else {
        scenarioTicks = int(num(1, 1000000));
        scenarioMode = argv[++i];
        scenarioWorkers = int(num(1, 64));
        scenarioPhases = int(num(1, 16));
        if (scenarioMode != "legacy" && scenarioMode != "handshake") { std::fprintf(stderr, "--handshake-scenario: mode must be legacy|handshake\n"); badArg = true; }
      }
    }
    else if (!std::strcmp(argv[i], "--dim") && i + 1 < argc) cfg.dimension = argv[++i];
    else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) logPath = argv[++i];
    else if (!std::strcmp(argv[i], "--scripts") && i + 1 < argc) scriptsMode = argv[++i];
    else if (!std::strcmp(argv[i], "--pack") && i + 1 < argc) pack = argv[++i];
    else if (!std::strcmp(argv[i], "--resource-pack") && i + 1 < argc) extraPacks.emplace_back(argv[++i]);
    else if (!std::strcmp(argv[i], "--mute")) mute = true;
    else if (!std::strcmp(argv[i], "--renderer") && i + 1 < argc) backend = argv[++i];
    else if (!std::strcmp(argv[i], "--vk-validation")) vkValidation = true;
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) {
      seedGiven = true;
      const char* s = argv[++i];
      const auto [end, ec] = std::from_chars(s, s + std::strlen(s), cfg.seed);
      if (ec != std::errc{} || *end != '\0') { std::fprintf(stderr, "--seed: '%s' is not an unsigned 64-bit number\n", s); badArg = true; }
    }
    else if (!std::strcmp(argv[i], "--noise-preview") && i + 1 < argc) { preview.graphFile = argv[++i]; runPreview = true; }
    else if (!std::strcmp(argv[i], "--preview-out") && i + 1 < argc) preview.outFile = argv[++i];
    else if (!std::strcmp(argv[i], "--preview-output") && i + 1 < argc) preview.output = argv[++i];
    else if (!std::strcmp(argv[i], "--preview-size")) preview.size = int(num(1, 16384));
    else if (!std::strcmp(argv[i], "--preview-scale")) preview.scale = float(num(1e-6, 1e6));
    else if (!std::strcmp(argv[i], "--preview-y")) preview.y = float(num(-1e6, 1e6));
    else if (!std::strcmp(argv[i], "--selftest")) runSelftest = true;
    else if (!std::strcmp(argv[i], "--perf-headless")) { runPerf = true; if (i + 1 < argc && argv[i + 1][0] != '-') perfJson = argv[++i]; }
    else if (!std::strcmp(argv[i], "--bench-scene") && i + 1 < argc) {
      cfg.benchScene = argv[++i];
      if (cfg.benchScene != "default" && cfg.benchScene != "circuits" && cfg.benchScene != "display" && cfg.benchScene != "framebuffer" && cfg.benchScene != "redirector" && cfg.benchScene != "wall-pixel" && cfg.benchScene != "pattern-wall" && cfg.benchScene != "sawtooth" && cfg.benchScene != "wire-torture") { std::fprintf(stderr, "--bench-scene: default|circuits|display|framebuffer|redirector|wall-pixel|pattern-wall|sawtooth|wire-torture\n"); badArg = true; }
    }
    else if (!std::strcmp(argv[i], "--version")) { std::printf("LithoKraft %s (LithoLattice engine)\n", LITHOKRAFT_VERSION); return 0; }
    else {
      std::printf("usage: LithoKraft [--version] [--selftest] [--dim wafer|organic] [--size chunks] "
                  "[--tps 32|64|128|256|512] [--tps-unlock (debug: --tps up to 8192, powers of 2)] [--no-euv] [--radius chunks] [--seconds s] [--vsync] [--display windowed|maximized|borderless|fullscreen] [--monitor N] [--resolution WxH] [--refresh Hz] [--log file] "
                  "[--pack pack_64|pack_32] [--resource-pack dir]... [--scripts source|exe|dir] [--mute] [--renderer software|gl33|vulkan] [--bench seconds] [--bench-path orbit|stream|flyover] [--bench-scene default|circuits|display|framebuffer|redirector|wall-pixel|pattern-wall|sawtooth|wire-torture] [--scene-size n] [--scene-rows 1-64 (wall-pixel <= 16, sawtooth)] [--pattern checkers|circles (pattern-wall)] [--screen-orient flat|vertical] [--perf-headless [json]] [--soak minutes] [--bench-json file] [--present fifo|mailbox|immediate] [--save-dir dir] [--no-save] [--mcp-stdio] "
                  "[--vk-validation] [--vk-arena host|device|rebar] [--gl-arena on|off] [--seed n] [--noise-preview graph.json [--preview-out f.png] "
                  "[--preview-output name] [--preview-size px] [--preview-scale blocks/px] [--preview-y y]]\n");
      return 0;
    }
  }
  if (badArg) return 2;
  // Player settings (config/settings.json next to config/input.json): CLI flags win.
  const std::string settingsPath = (dataRoot(argv[0]) / "config" / "settings.json").lexically_normal().string();
  GameSettings settings = GameSettings::load(settingsPath);
  if (!hsGiven) { cfg.timingHandshake = settings.timingHandshake; cfg.handshakePhases = settings.handshakePhases; }
  if (!tpsGiven) cfg.tps = settings.tps;  // load() only yields valid modes
  if (!radiusGiven) cfg.tickRadius = settings.tickRadius;
  vsync = vsync || settings.vsync;
  if (!displayGiven) {
    DisplayMode mode = display0.mode;
    if (parseDisplayMode(kSettingsDisplayModes[settings.displayMode], mode)) display0.mode = mode;
  }
  if (mute) settings.muted = true;
  cfg.version = LITHOKRAFT_VERSION;
  cfg.settings = settings;
  cfg.settingsPath = settingsPath;
  const ScriptsDirChoice scripts = findScriptsDir(argv[0], scriptsMode);
  cfg.scriptsDir = scripts.dir;
  // Saves: <data root>/saves/world by default; --bench never loads or saves (repeatable runs).
  if (!noSave && benchSeconds <= 0) cfg.saveDir = saveDir.empty() ? (dataRoot(argv[0]) / "saves" / "world").lexically_normal().string() : saveDir;
  if (runHandshakeTest) return runHandshakeTests();
  if (runScenario) return runHandshakeScenario(scenarioTicks, scenarioMode, scenarioWorkers, scenarioPhases);
  if (wantControlLatency) return runControlLatency();
  if (wantControlTest) return runControlTests();
  if (wantDemoTest) return runDemoTests();
  if (wantDemoDump) return runDemoDump(demoFile, demoTicksGiven, demoTicksLo, demoTicksHi);
  if (wantDemoVerify) return runDemoVerify(demoFile);
  if (wantControlScenario) return runControlScenario(controlScenarioTicks, controlScenarioMode);
  if (runPerf) {
    BenchSceneConfig bench;
    bench.scene = cfg.benchScene;
    bench.size = cfg.sceneSize;
    bench.rows = cfg.sceneRows;
    bench.pattern = cfg.pattern;
    bench.timingHandshake = cfg.timingHandshake;
    bench.handshakePhases = cfg.handshakePhases;
    return runPerfHeadless(perfJson, bench);
  }
  if (runSelftest) return runSelfTests(cfg.scriptsDir, packStack(findAssetDirs(argv[0]), "pack_64", {}), cfg.timingHandshake);
  if (runPreview) {  // headless tool: no window, no log file
    preview.seed = cfg.seed;
    return runNoisePreview(preview);
  }
  // F7 cycles kTickModes (32..256); the CLI also takes 512 (kMaxDomainHz) for stress benches.
  // --tps-unlock (debug / bench only): any power of two up to 8192 TPS, the enthusiast-tier probe.
  const bool tpsUnlocked = tpsUnlock && cfg.tps >= 32 && cfg.tps <= 8192 && (cfg.tps & (cfg.tps - 1)) == 0;
  if (!tpsUnlocked && cfg.tps != kMaxDomainHz && std::find(std::begin(kTickModes), std::end(kTickModes), cfg.tps) == std::end(kTickModes)) {
    std::fprintf(stderr, "--tps must be 32, 64, 128, 256 or 512\n");  // stderr: the log file is not open yet
    logError("--tps must be 32, 64, 128, 256 or 512"); return 1;
  }
  if (backend != "software" && backend != "gl33" && backend != "vulkan") { logError("--renderer must be software, gl33 or vulkan"); return 1; }

  if (logOpenFile(logPath)) logInfo("LithoKraft %s (LithoLattice engine), log file %s (previous run: .prev.log)", LITHOKRAFT_VERSION, logPath.c_str());
  else logError("could not open log file %s; console only", logPath.c_str());
  // SV-H2 seed precedence: an existing world's persisted seed (level.json in
  // the save dir) WINS over --seed and over the default. This must run here —
  // before Game construction — because Game's constructor builds the
  // dimensions from cfg.seed in its initializer list, before any save is
  // read; a late override would regenerate untouched (dirty-only-saved)
  // columns from the wrong seed under the saved ones. The Wafer is pinned
  // to seed 0 inside worldgen and is unaffected either way.
  if (!cfg.saveDir.empty()) {
    std::string seedErr;
    if (const auto persisted = WorldSave::readWorldSeed(cfg.saveDir, &seedErr)) {
      if (seedGiven && *persisted != cfg.seed)
        logError("save: --seed %llu DISAGREES with the saved world's seed %llu in '%s'; using the SAVED seed (an existing world must not regenerate under a new seed)",
                 (unsigned long long)cfg.seed, (unsigned long long)*persisted, cfg.saveDir.c_str());
      cfg.seed = *persisted;
    } else if (!seedErr.empty()) {
      // Present but unreadable after recovery: warn loudly and fall back to
      // the CLI/default seed. (level.json alone gates no region data — the
      // blocks.names fail-closed path in WorldSave is the data gate.)
      logError("save: world metadata in '%s' is unreadable (%s); falling back to %s seed %llu", cfg.saveDir.c_str(), seedErr.c_str(),
               seedGiven ? "the --seed" : "the default", (unsigned long long)cfg.seed);
    }
  }
  if (scripts.dir.empty()) logError("scripts: no scripts/ folder (%s)", scripts.reason.c_str());
  else logInfo("scripts: %s (%s; the F10 editor saves here)", scripts.dir.c_str(), scripts.reason.c_str());

  Display display = createDisplay(backend, vsync, vkValidation, presentMode, display0, vkArena, glArena);
  if (!display.window || !display.renderer) return 1;
  Window* const window = display.window.get();
  IRenderer* const renderer = display.renderer.get();
  logInfo("renderer: %s | %s", renderer->name(), renderer->deviceInfo().c_str());
  const AssetDirs assets = findAssetDirs(argv[0]);
  setWindowIcon(*window, assets);
  if (assets.engine.empty()) logError("engine assets/ not found; built-in shaders");
  if (assets.game.empty()) logError("LithoKraft/assets/ not found; every block uses the magenta missing texture, built-in theme and generators");
  // PBR material registry (phase 2): the game resolves the data-file path, the engine loads it
  // (initMaterialRegistry). A missing file is graceful: log + the built-in default, the same
  // degrade-loudly convention as audio's "no output device; continuing muted". The registry
  // outlives the game (chunk saves resolve string IDs through it on the I/O thread).
  litholattice::MaterialRegistry materials;
  {
    std::string matErr;
    const std::string matPath = assets.engine.empty()
                                    ? ""
                                    : (std::filesystem::path(assets.engine) / "materials" / "lithokraft.materials").string();
    if (litholattice::initMaterialRegistry(materials, matPath, &matErr))
      logInfo("materials: %zu materials from %s", materials.size(), matPath.c_str());
    else
      logError("materials: %s; continuing with the built-in default", matErr.c_str());
  }
  cfg.materials = &materials;
  const std::vector<std::filesystem::path> modPacks = scanMods(argv[0]);
  ResourceManager resources(packStack(assets, pack, extraPacks, modPacks));  // outlives the game (mesh jobs)
  resources.reload();
  resources.apply(*renderer);
  AudioEngine audio;  // outlives the output device and the game
  std::unique_ptr<AudioOutput> audioOut;
  if (mute) {
    audio.setEnabled(false);
    logInfo("audio: muted (--mute)");
  } else {
    std::string err;
    audioOut = openAudioOutput(audio, &err);
    if (audioOut) logInfo("audio: %s", audioOut->description().c_str());
    else { audio.setEnabled(false); logError("audio: no output device (%s); continuing muted", err.c_str()); }
  }
  {
    JobPool pool(std::max(1u, std::max(2u, std::thread::hardware_concurrency()) - 2));
    InputSystem input(loadBindings(argv[0]), inputContexts());
    window->loadGamepadMappings((dataRoot(argv[0]) / "config" / "gamecontrollerdb.txt").string());  // optional
    WindowClipboard clipboard(*window);  // UI text fields copy / paste through the OS; outlives the game
    Game game(cfg, *renderer, pool, resources, audio, input);
    if (!game.ok()) return 1;
    // --mcp-stdio (P2c, F-M9): the engine MCP server over stdin/stdout on
    // its own thread, for the process lifetime. Default OFF: no flag, no
    // server, no thread, no behaviour change. The backends are the game's
    // existing adapters (Game::mcpBackends); the `mcp grant|revoke|agent`
    // console commands are registered by setMcpServer. Shutdown is
    // sequenced after the frame loop below: stop + join BEFORE the Game
    // is destroyed, because the backends point into it.
    std::unique_ptr<litholattice::mcp::McpServer> mcpServer;
    std::thread mcpThread;
#if !defined(_WIN32)
    std::unique_ptr<McpStdioTransport> mcpTransport;
    int mcpSavedStdoutFd = -1;
#endif
    if (mcpStdio) {
      mcpServer = std::make_unique<litholattice::mcp::McpServer>(game.mcpBackends());
      mcpServer->setLogSink([](std::string_view line) {
        std::fprintf(stderr, "[mcp] %.*s\n", int(line.size()), line.data());
      });
      game.setMcpServer(mcpServer.get());
#if !defined(_WIN32)
      // stdout belongs to JSON-RPC now: save the real stdout for the
      // transport, then point fd 1 at stderr so logInfo (which writes to
      // stdout) and any stray printf cannot corrupt the protocol stream.
      std::fflush(stdout);
      mcpSavedStdoutFd = ::dup(STDOUT_FILENO);
      if (mcpSavedStdoutFd >= 0) ::dup2(STDERR_FILENO, STDOUT_FILENO);
      const int outFd = mcpSavedStdoutFd >= 0 ? mcpSavedStdoutFd : STDOUT_FILENO;
      mcpTransport = std::make_unique<McpStdioTransport>(outFd);
      litholattice::mcp::McpServer* srv = mcpServer.get();
      McpStdioTransport* tr = mcpTransport.get();
      mcpThread = std::thread([srv, tr] { srv->runStdio(*tr); });
#else
      // Windows: the engine's own stdio transport (blocking std::cin);
      // the thread is detached at shutdown — process exit ends it. The
      // poll-based clean join above is the POSIX path this build targets.
      litholattice::mcp::McpServer* srv = mcpServer.get();
      mcpThread = std::thread([srv] { srv->runStdio(); });
#endif
      logWarn("mcp: stdio server running (agent starts with NO permissions; approve with `mcp grant <perm...>` in the console)");
    }
    game.setClipboard(&clipboard);
    game.setDisplayModeApplier([window](DisplayMode mode) {
      DisplayDesc d = window->display();
      d.mode = mode;
      window->setDisplay(d);
    });
    // The launch menu opens first (world loaded, sim paused, cinematic camera); the bench
    // harness drives the camera itself and skips it. The loop syncs cursor capture from
    // game.wantsCursorCaptured() every frame.
    if (benchSeconds <= 0) game.enterMenu();

    const auto tStart = Clock::now();
    auto tLast = tStart, tTitle = tStart, tPerf = tStart;
    constexpr double kBenchWarmup = 3.0;  // world load + first meshes settle before measuring
    if (benchSeconds > 0) runSeconds = kBenchWarmup + benchSeconds;
    if (benchSeconds > 0) game.setBenchPath(benchPath);
    const std::string rname = renderer->name();
    const std::string present = rname == "Vulkan"    ? renderer->presentMode()
                                : rname == "software" ? renderer->presentMode() + (vsync ? " (swap-interval-1)" : " (swap-interval-0)")
                                                      : (vsync ? "swap-interval-1" : "swap-interval-0");
    logInfo("present: %s (%s)", present.c_str(), renderer->name());
    BenchRun br;
    br.memStart = processWorkingSetBytes();
    double msInput = 0, msUpdate = 0, msRender = 0, msPresent = 0;
    std::vector<double> perfMs, benchMs;  // this second's frame times (p99), the bench's
    int frames = 0, perfFrames = 0, hitches = 0; double worstMs = 0, perfSumMs = 0, perfMaxMs = 0, avgFrameMs = 0;
    bool wasFocused = window->focused();
    // Frame pacer (render.fps_cap, D-M3 game-side wiring 2026-10-13):
    // configured from Settings at init (0 = off, vsync-only — the default)
    // and reconfigured live whenever the settings value changes (the
    // cvar/UI flow lands in Game::settings() via onSettingsChanged).
    // beginFrame() at the loop top, endFrameAndWait() right after swap.
    // Presentation only: the pacer never feeds the sim.
    litholattice::FramePacer pacer;
    pacer.setCapFps(settings.fpsCap);
    int appliedFpsCap = settings.fpsCap;
    if (appliedFpsCap > 0) logInfo("fps cap: %d FPS (render.fps_cap)", appliedFpsCap);
    while (!window->shouldClose()) {
      if (const int cap = game.settings().fpsCap; cap != appliedFpsCap) {
        pacer.setCapFps(cap);
        appliedFpsCap = cap;
        logInfo("fps cap: %d FPS (render.fps_cap, live)", cap);
      }
      pacer.beginFrame();
      window->pollDisplayToggle();  // Alt+Enter
      const auto tIn = Clock::now();
      input.update(window->input());  // pumps OS events; actions for this frame
      const auto now = Clock::now();
      msInput = msBetween(tIn, now);
      const float dt = float(std::min(0.1, secondsBetween(tLast, now)));
      const double frameMs = msBetween(tLast, now);
      worstMs = std::max(worstMs, frameMs);
      tLast = now;
      if (frames + perfFrames > 0) {  // hitch: a frame over 2x the running average (and over 10 ms)
        if (avgFrameMs > 0 && frameMs > 2.0 * avgFrameMs && frameMs > 10.0 && ++hitches <= 3)  // max 3 lines/s
          logInfo("hitch: frame %.1f ms (avg %.2f ms)", frameMs, avgFrameMs);
        avgFrameMs = avgFrameMs > 0 ? avgFrameMs * 0.95 + frameMs * 0.05 : frameMs;
        perfSumMs += frameMs; perfMaxMs = std::max(perfMaxMs, frameMs); ++perfFrames; perfMs.push_back(frameMs);
        if (benchSeconds > 0 && secondsBetween(tStart, now) > kBenchWarmup) {
          // msUpdate / msRender still hold the previous frame's split (this frameMs ends here).
          benchMs.push_back(frameMs);
          br.minSum += frameMs; ++br.minFrames;
          br.input.push_back(msInput); br.update.push_back(msUpdate - game.lastUploadMs()); br.upload.push_back(game.lastUploadMs());
          br.render.push_back(msRender); br.present.push_back(msPresent); br.uploadBytes.push_back(double(game.lastUploadBytes()));
          br.secInput += msInput; br.secUpdate += msUpdate - game.lastUploadMs(); br.secUpload += game.lastUploadMs();
          br.secRender += msRender; br.secPresent += msPresent; br.secBytes += double(game.lastUploadBytes()); ++br.secFrames;
        }
      }
      if (runSeconds > 0 && secondsBetween(tStart, now) > runSeconds) break;

      int w, h; window->framebufferSize(w, h);
      if (benchSeconds > 0) game.setBenchCamera(float(secondsBetween(tStart, now)));
      const auto tUp = Clock::now();
      game.update(dt, w, h);
      if (benchSeconds > 0) game.setBenchCamera(float(secondsBetween(tStart, now)));
      const auto tRender = Clock::now();
      msUpdate = msBetween(tUp, tRender);
      if (const bool foc = window->focused(); foc != wasFocused) {
        wasFocused = foc;
        game.onFocusChanged(foc);
        if (foc) window->setCursorCaptured(game.wantsCursorCaptured());  // re-apply: the OS dropped it
      }
      if (game.quitRequested()) window->requestClose();
      if (game.wantsCursorCaptured() != window->cursorCaptured()) window->setCursorCaptured(game.wantsCursorCaptured());
      if (w > 0 && h > 0) {
        game.uploadEntities();
        FrameParams fp = game.frameParams(w, h);
        fp.timeSec = float(secondsBetween(tStart, now));
        renderer->renderFrame(fp);
        game.capturePendingScreenshot();  // `screenshot` lands here, pre-present
      }
      const auto tPresent = Clock::now();
      if (display.presenter && display.software) {  // software backend: blit the CPU
        const SoftwareFramebuffer& fb = display.software->framebuffer();  // frame, then swap
        if (fb.width > 0 && fb.height > 0) display.presenter->present(fb.pixels.data(), int(fb.width), int(fb.height), w, h);
      }
      window->swap();
      pacer.endFrameAndWait();  // render.fps_cap: sleep to the frame deadline (no-op when uncapped)
      msPresent = msBetween(tPresent, Clock::now());  // present = blit + swap (swap only on GPU backends)
      msRender = msBetween(tRender, tPresent);
      ++frames;

      const double since = secondsBetween(tTitle, now);
      if (since >= 0.5) {
        window->setTitle(game.statusLine(frames / since, worstMs));
        game.setFrameStats(float(frames / since), float(since * 1000.0 / std::max(1, frames)), float(worstMs));
        frames = 0; worstMs = 0; tTitle = now;
      }
      const double perfSince = secondsBetween(tPerf, now);
      if (perfSince >= 1.0) {
        logInfo("%s p99=%.2fms hitches=%d", game.perfLine(perfFrames / perfSince, perfFrames ? perfSumMs / perfFrames : 0.0, perfMaxMs).c_str(),
                percentile(perfMs, 0.99), hitches);
        if (benchSeconds > 0) {
          const uint64_t mem = processWorkingSetBytes();
          br.memPeak = std::max(br.memPeak, mem);
          const auto ssOpt = game.simStats();
          if (!ssOpt) continue;
          const SimStats& ss = ssOpt->get();
          if (secondsBetween(tStart, now) > kBenchWarmup) br.tick.push_back(ss.tickMs);
          const double f = std::max(1, br.secFrames);
          std::string streamPart;
          if (BenchStreamer* bs = game.benchStreamer()) {
            const double genAvg = MsSamples::avg(bs->genMs.second), genP99 = MsSamples::pct(bs->genMs.second, 0.99);
            const double meshAvg = MsSamples::avg(bs->meshMs.second);
            const BenchStreamer::Second s = bs->takeSecond();
            char buf[256];
            std::snprintf(buf, sizeof buf, " stream gen=%d cols (%.2f/%.2fms) meshed=%d subs (%.3fms) dropped=%d loaded=%d", s.generated,
                          genAvg, genP99, s.meshed, meshAvg, s.dropped, s.loaded);
            streamPart = buf;
          }
          logInfo("bench-sec frame: input=%.3f update=%.3f upload=%.3f render=%.3f present=%.3f ms, upload=%.0f B/frame, tick=%.3f ms, ram=%.1f MB%s",
                  br.secInput / f, br.secUpdate / f, br.secUpload / f, br.secRender / f, br.secPresent / f, br.secBytes / f, ss.tickMs, double(mem) / 1048576.0,
                  streamPart.c_str());
          if (benchPath == "soak" && br.minFrames > 0 && secondsBetween(tStart, now) - kBenchWarmup >= 60.0 * (br.minuteNo + 1)) {
            br.minuteFrame.push_back(br.minSum / br.minFrames); br.minuteRam.push_back(double(mem) / 1048576.0);
            logInfo("soak minute=%d frame_avg=%.3fms ram=%.1fMB (drift vs minute 1: %+.3f ms, %+.1f MB)", br.minuteNo + 1, br.minuteFrame.back(),
                    br.minuteRam.back(), br.minuteFrame.back() - br.minuteFrame.front(), br.minuteRam.back() - br.minuteRam.front());
            br.minSum = 0; br.minFrames = 0; ++br.minuteNo;
          }
          br.secInput = br.secUpdate = br.secUpload = br.secRender = br.secPresent = br.secBytes = 0; br.secFrames = 0;
        }
        perfMs.clear(); perfFrames = 0; hitches = 0; perfSumMs = perfMaxMs = 0; tPerf = now;
      }
    }
    // MCP shutdown (P2c, F-M9): stop + join the stdio thread and drop the
    // server BEFORE the Game (and its adapters) is destroyed at scope
    // exit; restore the real stdout for the bench summary / exit path.
    if (mcpServer) {
      game.setMcpServer(nullptr);
#if !defined(_WIN32)
      if (mcpTransport) mcpTransport->requestStop();
      if (mcpThread.joinable()) mcpThread.join();
#else
      if (mcpThread.joinable()) mcpThread.detach();
#endif
      mcpServer.reset();
#if !defined(_WIN32)
      mcpTransport.reset();
      if (mcpSavedStdoutFd >= 0) {
        std::fflush(stdout);
        ::dup2(mcpSavedStdoutFd, STDOUT_FILENO);
        ::close(mcpSavedStdoutFd);
        mcpSavedStdoutFd = -1;
      }
#endif
    }
    if (benchSeconds > 0) {
      br.memEnd = processWorkingSetBytes();
      const double avg = avgOf(benchMs), p50 = percentile(benchMs, 0.5), p99 = percentile(benchMs, 0.99), mx = percentile(benchMs, 1.0);
      BenchStreamer* bs = game.benchStreamer();
      const double genAvg = bs ? MsSamples::avg(bs->genMs.run) : 0, genP99 = bs ? MsSamples::pct(bs->genMs.run, 0.99) : 0;
      const double meshAvg = bs ? MsSamples::avg(bs->meshMs.run) : 0, meshP99 = bs ? MsSamples::pct(bs->meshMs.run, 0.99) : 0;
      const double genRate = bs ? double(bs->totalGenerated) / benchSeconds : 0, meshRate = bs ? double(bs->totalMeshed) / benchSeconds : 0;
      logInfo("bench renderer=%s present=%s path=%s seconds=%.0f frames=%zu avg=%.3fms p50=%.3fms p99=%.3fms max=%.3fms "
              "input=%.3f update=%.3f upload=%.3f render=%.3f present=%.3f ms upload=%.0f B/frame tick=%.3f/%.3fms gen=%.1f cols/s (%.2f/%.2fms) "
              "mesh=%.1f subs/s (%.3f/%.3fms) ram=%.1f->%.1f MB (peak %.1f)",
              renderer->name(), present.c_str(), benchPath.c_str(), benchSeconds, benchMs.size(), avg, p50, p99, mx, avgOf(br.input),
              avgOf(br.update), avgOf(br.upload), avgOf(br.render), avgOf(br.present), avgOf(br.uploadBytes), avgOf(br.tick), percentile(br.tick, 0.99), genRate,
              genAvg, genP99, meshRate, meshAvg, meshP99, double(br.memStart) / 1048576.0, double(br.memEnd) / 1048576.0,
              double(br.memPeak) / 1048576.0);
      if (!br.minuteFrame.empty())
        logInfo("soak drift: frame %+.3f ms, ram %+.1f MB over %zu minutes (same-path minutes: %+.3f ms)", br.minuteFrame.back() - br.minuteFrame.front(),
                br.minuteRam.back() - br.minuteRam.front(), br.minuteFrame.size(),
                br.minuteFrame.size() >= 3 ? br.minuteFrame.back() - br.minuteFrame[br.minuteFrame.size() % 2 ? 0 : 1] : 0.0);
      std::printf("BENCH %s present=%s display=%s path=%s frames=%zu avg=%.3f ms p99=%.3f ms\n", renderer->name(), present.c_str(),
                  displayModeName(window->display().mode), benchPath.c_str(), benchMs.size(), avg, p99);
      if (!benchJson.empty()) {
        if (std::FILE* f = openForWrite(benchJson.c_str())) {
          std::fprintf(f,
                       "{\n  \"renderer\": \"%s\", \"present\": \"%s\", \"display\": \"%s\", \"path\": \"%s\", \"seconds\": %.0f, \"frames\": %zu,\n"
                       "  \"frame_ms\": {\"avg\": %.4f, \"p50\": %.4f, \"p99\": %.4f, \"max\": %.4f},\n"
                       "  \"main_thread_ms\": {\"input\": %.4f, \"update\": %.4f, \"upload\": %.4f, \"render\": %.4f, \"present\": %.4f},\n"
                       "  \"upload_bytes_per_frame\": %.0f, \"sim_tick_ms\": {\"avg\": %.4f, \"p99\": %.4f},\n"
                       "  \"stream\": {\"columns_per_s\": %.2f, \"gen_ms_avg\": %.4f, \"gen_ms_p99\": %.4f, \"subchunks_per_s\": %.2f, "
                       "\"mesh_ms_avg\": %.4f, \"mesh_ms_p99\": %.4f},\n"
                       "  \"ram_mb\": {\"start\": %.1f, \"end\": %.1f, \"peak\": %.1f},\n  \"soak_drift\": {\"frame_ms\": %.4f, \"ram_mb\": %.1f, \"minutes\": %zu}\n}\n",
                       renderer->name(), present.c_str(), displayModeName(window->display().mode), benchPath.c_str(), benchSeconds, benchMs.size(), avg, p50, p99, mx, avgOf(br.input),
                       avgOf(br.update), avgOf(br.upload), avgOf(br.render), avgOf(br.present), avgOf(br.uploadBytes), avgOf(br.tick), percentile(br.tick, 0.99),
                       genRate, genAvg, genP99, meshRate, meshAvg, meshP99, double(br.memStart) / 1048576.0, double(br.memEnd) / 1048576.0,
                       double(br.memPeak) / 1048576.0,
                       br.minuteFrame.empty() ? 0.0 : br.minuteFrame.back() - br.minuteFrame.front(),
                       br.minuteRam.empty() ? 0.0 : br.minuteRam.back() - br.minuteRam.front(), br.minuteFrame.size());
          std::fclose(f);
        } else {
          logError("bench: cannot write %s", benchJson.c_str());
        }
      }
    }
  }  // game (sim thread, mesh jobs) shuts down before the renderer and window
  display.renderer.reset();
  logInfo("exit");
  logCloseFile();
  return 0;
}
