// Headless checks (--selftest): signal logic, audio, input, dimension load/unload, demo circuit, mesher,
// overlay geometry and a headless Vulkan instance (skipped without a Vulkan loader).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <cstdio>
#ifdef _WIN32
#include <process.h>  // _getpid: per-run unique temp dirs (G-M7)
#define getpid _getpid
#else
#include <unistd.h>  // getpid: per-run unique temp dirs (G-M7)
#endif
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include "litholattice/core/log.h"
#include "litholattice/mods/mod_interface.h"  // kPerm* for the script sandbox
#include "lithokraft/content/appearance.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/items.h"
#include "lithokraft/game/inventory.h"
#include "lithokraft/content/content_selftest.h"
#include "lithokraft/game/input_selftest.h"
#include "scripts_dir.h"
#include "selftest.h"
#include "handshake_test.h"
#include "control_stream_test.h"
#include "demo_record_test.h"
#include "litholattice/core/threading.h"
#include "litholattice/render/mesher.h"
#include "litholattice/assets/assets_selftest.h"
#include "lithokraft/game/resource_manager.h"
#include "litholattice/render/hud_ui_renderer.h"
#include "lithokraft/game/dimensions.h"
#include "lithokraft/game/chunk_streamer.h"
#include "lithokraft/game/world_save.h"
#include "litholattice/storage/anvil_region.h"    // P3-2 C-L3 test
#include "litholattice/storage/file_contract.h"   // P3-2 C-L2 test
#include "litholattice/world/light_engine.h"      // P3-2 B-L5 test
#include "litholattice/procgen/shrubs.h"          // spawn garden sprite pins (2026-10-13)
#include "litholattice/render/block_materials.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render_vk/vulkan_renderer.h"
#include "litholattice/audio/audio_selftest.h"
#include "litholattice/input/input_selftest.h"
#include "litholattice/procgen/procgen_selftest.h"
#include "litholattice/ui/ui_selftest.h"
#include "lithokraft/game/game_ui_selftest.h"
#include "lithokraft/game/wiring_selftest.h"
#include "lithokraft/game/sprite_host.h"
#include "lithokraft/game/settings.h"        // A4: render.fps_cap Settings field
#include "lithokraft/game/settings_cvars.h"  // A4: render.fps_cap cvar row
#include "litholattice/console/concmd.h"     // A4: ConsoleRegistry for the cvar flow check
#include "litholattice/render/frame_pacer.h"  // A4: FramePacer drive semantics
#include "lithokraft/game/ai_host.h"         // A6: AiWorld production host smoke test
#include "lithokraft/game/deceitor.h"        // Deceitor mob behaviour test (2026-10-14)
#include "lithokraft/game/interchange.h"    // Interchange settlements test (2026-10-14)
#include "lithokraft/game/player_locomotion.h"
#include "lithokraft/content/component.h"
#include "lithokraft/game/world_save.h"
#include "litholattice/script/script_engine.h"
#include "lithokraft/content/signal.h"
#include "lithokraft/content/item_use.h"
#include "litholattice/projectile/projectile.h"
#include "litholattice/world/dimension.h"

namespace lithokraft {

static std::string stubDimension() { return "selftest"; }

/// Per-run unique temp directory (P2b-2, review G-M7): the suite's fixed
/// shared temp paths (save/scripts/logs) collided when selftest ran in
/// parallel (ctest -j2, or a manual run beside ctest), producing flaky
/// cross-talk failures. pid + steady-clock stamp + a per-process counter
/// make every run's dirs its own.
static std::filesystem::path uniqueTempDir(const char* base) {
  static std::atomic<unsigned> seq{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         (std::string(base) + "-" + std::to_string(::getpid()) + "-" + std::to_string(stamp) + "-" +
          std::to_string(seq.fetch_add(1)));
}

int runSelfTests(const std::string& scriptsDir, const std::vector<std::filesystem::path>& packDirs, bool timingHandshake) {
  int fails = runSignalSelfTest() + runContentSelfTest() + runAudioSelfTest() + runInputSelfTest() + runGameInputSelfTest() + runAssetsSelfTest();
  auto check = [&](bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };
  JobPool pool(2);
  {
    std::printf("  (the next two job errors are expected)\n"); std::fflush(stdout);
    std::atomic<int> ran{0};
    JobBatch batch(pool);
    for (int i = 0; i < 8; ++i) batch.submit([&ran, i] { ++ran; if (i == 3) throw std::runtime_error("selftest"); });
    const int failed = batch.wait();
    check(failed == 1 && ran == 8, "job batch: a throwing job is caught and counted, the batch still completes");
    pool.submit([] { throw 42; });
    pool.waitIdle();
    check(pool.failedJobs() == 1, "job pool: a throwing job does not terminate, waitIdle() still returns");
  }
  fails += procgen::runProcgenSelfTest(pool);
  fails += ui::runUiSelfTest();
  fails += runGameUiSelfTest();
  fails += runWiringSelfTest();
  fails += runSpriteHostSelfTest(packDirs);
  fails += runLocomotionSelfTest();
  {  // render.fps_cap (A4 / brief B7, 2026-10-13): the game-side wiring of
     // the engine FramePacer — Settings field, JSON persistence, the
     // render.fps_cap cvar flow, and pacer drive semantics. Deterministic
     // by construction: the pacer's returned sleep is computed from
     // explicit time points; the single measured check is bounded wide.
    using litholattice::FramePacer;
    check(GameSettings{}.fpsCap == 0, "fps cap: Settings default is 0 (pacer off, vsync-only)");
    {  // JSON round-trip + load-side clamp (0..1000).
      const auto dir = uniqueTempDir("lithokraft-fpscap");
      const std::string path = (dir / "settings.json").string();
      GameSettings out;
      out.fpsCap = 60;
      const bool saved = out.save(path);
      check(saved && GameSettings::load(path).fpsCap == 60, "fps cap: settings.json round-trips fpsCap=60");
      std::ofstream raw(path, std::ios::trunc);
      raw << "{\"fpsCap\": 5000}\n";
      raw.close();
      check(GameSettings::load(path).fpsCap == 1000, "fps cap: settings.json load clamps 5000 -> 1000");
      std::ofstream raw2(path, std::ios::trunc);
      raw2 << "{\"fpsCap\": -5}\n";
      raw2.close();
      check(GameSettings::load(path).fpsCap == 0, "fps cap: settings.json load clamps -5 -> 0 (off)");
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }
    {  // Pacer semantics: off = zero wait; cap clamps; on-time frame waits
       // the computed remainder of its interval; overrun waits nothing.
      FramePacer off;
      const auto t0 = FramePacer::Clock::now();
      off.beginFrame(t0);
      check(!off.enabled() && off.capFps() == 0 && off.endFrameAndWait(t0) == FramePacer::Clock::duration::zero(),
            "fps cap: pacer default off — endFrameAndWait is a no-op");
      FramePacer p;
      p.setCapFps(5000);
      check(p.capFps() == 1000 && p.enabled(), "fps cap: pacer clamps cap to 1000");
      p.setCapFps(60);
      const auto start = FramePacer::Clock::now();
      p.beginFrame(start);
      const auto slept = p.endFrameAndWait(start);
      const double sleptMs = std::chrono::duration<double, std::milli>(slept).count();
      const double elapsedMs = std::chrono::duration<double, std::milli>(FramePacer::Clock::now() - start).count();
      check(sleptMs > 10.0 && sleptMs < 25.0 && elapsedMs >= 10.0 && elapsedMs < 100.0,
            "fps cap: a 60 FPS cap holds a fresh frame to its ~16.7 ms interval (bounded measured check)");
      p.beginFrame(start);
      check(p.endFrameAndWait(start + std::chrono::seconds(1)) == FramePacer::Clock::duration::zero(),
            "fps cap: an overrun frame waits nothing (no catch-up debt)");
      p.setCapFps(0);
      check(!p.enabled(), "fps cap: setCapFps(0) disables pacing (live change back to off)");
    }
    {  // Cvar flow: render.fps_cap registered by SettingsCvars; a console
       // set flows through the apply path with the clamp, like a UI edit.
      litholattice::console::ConsoleRegistry reg;
      GameSettings applied;
      SettingsCvars bridge([&](const GameSettings& s) { applied = s; }, [] { return GameSettings{}; });
      bridge.registerOn(reg);
      const auto* cv = reg.findCVar("render.fps_cap");
      check(cv && cv->value == "0", "fps cap: render.fps_cap cvar registered, default 0 (off)");
      check(reg.setCVar("render.fps_cap", "120") && applied.fpsCap == 120,
            "fps cap: cvar set 120 flows through the settings apply path");
      check(reg.setCVar("render.fps_cap", "99999") && applied.fpsCap == 1000,
            "fps cap: cvar set clamps to 1000 through the apply path");
      check(reg.setCVar("render.fps_cap", "0") && applied.fpsCap == 0,
            "fps cap: cvar set 0 turns the cap off through the apply path");
    }
  }
  fails += runAiHostSelfTest();
  fails += runDeceitorSelfTest();
  fails += runInterchangeSelfTest();
  DimensionRegistry reg = createDimensions();
  Dimension* wafer = reg.find("wafer");
  Dimension* ow = reg.find("organic");
  {  // B5 (2026-10-13, Team B worker2): cosmic + underwafer registered
     // with crust-level content; the void deliberately NOT registered
     // (infinite extent is not representable in a packPos key -- see
     // dimensions.h). Smoke: each new dimension's generator loads
     // fail-closed-clean and a real load() generates columns whose fill
     // matches its JSON's promise. PRESERVE this block in merges.
    Dimension* cos = reg.find("cosmic");
    Dimension* uw = reg.find("underwafer");
    check(cos && uw, "dimensions: cosmic and underwafer resolve in the registry (B5)");
    check(reg.find("void") == nullptr, "dimensions: void stays unregistered (infinite extent, no packPos representation)");
    check(reg.find("overworld") == ow, "dimensions: overworld alias still resolves to organic");
    check(cos && cos->generator && uw && uw->generator,
          "dimensions: cosmic/underwafer generators load without fail-closed errors");
    check(builtinWorldgenSource("cosmic") && builtinWorldgenSource("underwafer"),
          "dimensions: built-in fallback JSON exists for cosmic and underwafer");
    if (cos && cos->generator) {
      const Spawn cs = cos->load(pool, 2);
      const int sy = cos->chunks.surfaceY(int(cs.x), int(cs.z));
      check(cos->lastLoadFailures == 0 && sy >= 0 && sy < 256 && cs.y > float(sy) && cs.y <= float(sy) + 2.f,
            "dimensions: cosmic smoke load -- spawn on a crust surface inside the solid band (y < 256)");
      bool emptyAbove = true;
      for (const auto& [k, col] : cos->chunks.columns())
        for (int s = 16; s < kSubChunks; ++s)
          if (col->subs[s] && col->subs[s]->nonAir > 0) emptyAbove = false;
      check(emptyAbove, "dimensions: cosmic is all-air at y >= 256 (the Empty, as air until a medium field exists)");
    }
    if (uw && uw->generator) {
      const Spawn us = uw->load(pool, 2);
      const int sy = uw->chunks.surfaceY(int(us.x), int(us.z));
      check(uw->lastLoadFailures == 0 && sy > 0 && sy < 256 && us.y > float(sy) && us.y <= float(sy) + 2.f &&
                uw->chunks.get(int(us.x), sy, int(us.z)) == kStone,
            "dimensions: underwafer smoke load -- spawn on a stone surface in the deep band");
    }
  }

  const Spawn sp = wafer->load(pool, 6);
  {  // Spawn resolution (hands-on loop, headless portion): the Wafer spawn returned by load()
     // sits in air above a non-air surface cell at its own column.
    const int sx = int(sp.x), sz = int(sp.z), sy = wafer->chunks.surfaceY(sx, sz);
    std::printf("  spawn: wafer load(6) -> (%.1f, %.1f, %.1f), surfaceY at that column = %d\n", sp.x, sp.y, sp.z, sy);
    // Upper bound included (P2b-2, review G-L6): feet may stand at most
    // ~1 block above the surface cell's top (sy+1 is standing ON it).
    // The old check had no upper bound and passed with the G-M1 spawn
    // floating 11+ blocks up — it could not fail for that class of bug.
    check(sy >= 0 && sp.y > float(sy) && sp.y <= float(sy) + 2.f && wafer->chunks.get(sx, sy, sz) != kAir && wafer->chunks.get(sx, int(sp.y), sz) == kAir,
          "spawn: wafer spawn resolves to air just above the surface at its column (no float)");
  }
  {  // Spawn garden (2026-10-13, JD directive): waferSetup plants the two
     // seeded Litho Formula shrubs flanking the spawn -- seed 311 three
     // cells west, seed 314 three east, standing on the platform at the
     // spawn's own cell height. Exactly one of each exists in the whole
     // map, a fresh regeneration reproduces the same cells, and the pair
     // rides the save/load round trip below (checked explicitly against
     // the restored map there). PRESERVE this block in merges.
    const int sx = int(sp.x), sz = int(sp.z), py = int(sp.y);
    check(wafer->chunks.get(sx - 3, py, sz) == kFormulaShrub311,
          "spawn garden: seed-311 formula shrub stands west of the wafer spawn");
    check(wafer->chunks.get(sx + 3, py, sz) == kFormulaShrub314,
          "spawn garden: seed-314 formula shrub stands east of the wafer spawn");
    check(wafer->chunks.get(sx, py, sz) == kAir,
          "spawn garden: the spawn cell itself stays clear");
    int n311 = 0, n314 = 0;
    const StableId sid311 = blockRegistry().stableIdOf(kFormulaShrub311);
    const StableId sid314 = blockRegistry().stableIdOf(kFormulaShrub314);
    for (const auto& [k, col] : wafer->chunks.columns())
      for (int syy = 0; syy < kSubChunks; ++syy) {
        const auto& s = col->subs[syy];
        if (!s) continue;
        for (int i = 0; i < kSubChunkVolume; ++i) {
          if (s->blocks.at(i) == sid311) ++n311;
          if (s->blocks.at(i) == sid314) ++n314;
        }
      }
    check(n311 == 1 && n314 == 1, "spawn garden: exactly one 311 and one 314 shrub in the whole wafer");
    DimensionRegistry reg2 = createDimensions();
    Dimension* wafer2 = reg2.find("wafer");
    const Spawn sp2 = wafer2->load(pool, 6);
    check(int(sp2.x) == sx && int(sp2.z) == sz && int(sp2.y) == py &&
              wafer2->chunks.get(sx - 3, py, sz) == kFormulaShrub311 &&
              wafer2->chunks.get(sx + 3, py, sz) == kFormulaShrub314,
          "spawn garden: regeneration reproduces both shrubs at the same cells");
    // The sprites themselves (canonical bytes): ShrubParams defaults with
    // seed 311 / 314 -- one shared alpha mask, seed-painted colours.
    litholattice::procgen::ShrubParams p311; p311.seed = 311;
    litholattice::procgen::ShrubParams p314; p314.seed = 314;
    const auto spr311 = litholattice::procgen::generateShrubSprite(p311, litholattice::procgen::kPlantTextureSize);
    const auto spr314 = litholattice::procgen::generateShrubSprite(p314, litholattice::procgen::kPlantTextureSize);
    bool maskSame = spr311.size() == spr314.size(), colorDiff = false;
    int filled = 0;
    if (maskSame)
      for (size_t i = 0; i < spr311.size(); i += 4) {
        if ((spr311[i + 3] == 0) != (spr314[i + 3] == 0)) maskSame = false;
        if (spr311[i + 3]) { ++filled; if (spr311[i] != spr314[i] || spr311[i + 1] != spr314[i + 1] || spr311[i + 2] != spr314[i + 2]) colorDiff = true; }
      }
    check(maskSame && colorDiff && filled == 156,
          "spawn garden: seeds 311/314 share one alpha mask (156 filled px) and differ in colour");
  }
  SignalSim sig;
  sig.scan(wafer->chunks);
  for (int i = 0; i < 12; ++i) sig.tick(wafer->chunks, 2, 2, 12, [](int, int, int) {});
  int lamps = 0, lit = 0, clamps = 0, clit = 0, subs = 0, crustSubs = 0, surfaceSy = 0;
  for (auto& [k, col] : wafer->chunks.columns())
    for (int sy = 0; sy < kSubChunks; ++sy) {
      const auto& s = col->subs[sy];
      if (!s) continue;
      ++subs; if (sy <= 16) ++crustSubs; surfaceSy = std::max(surfaceSy, sy);
      const StableId lampSid = blockRegistry().stableIdOf(kLamp);  // StableId voxel currency
      for (int i = 0; i < kSubChunkVolume; ++i) {
        if (s->blocks.at(i) == lampSid) { ++lamps; lit += s->state[i] & 1; }
        if (blockRegistry().defByStableId(s->blocks.at(i)).role == SimRole::ColorLamp) { ++clamps; clit += stValue(s->state[i]) > 0; }
      }
    }
  check(lamps == 9 && lit == 7, "wafer demo: 9 plain lamps, 7 lit at launch (repeater, comparator, redirector rows included)");
  check(clamps == 35 && clit == 33, "wafer demo: lamp wall (32) + color lamps lit at launch");
  check(surfaceSy >= 15 && crustSubs <= 36 * 3 && subs <= 36 * 4,
        "wafer: surface near layer 256 of 768, only the crust is generated (empty sections skipped; feature spires may add a 4th section)");
  {  // Save round trip: blocks, cell states and signal-sim memory restore bit for bit, and the
     // restored world keeps ticking identically to the original.
    const auto dir = uniqueTempDir("lithokraft_save_selftest");  // G-M7: per-run unique
    std::filesystem::remove_all(dir);
    auto emptyLike = [](const ChunkMap& w) {
      ChunkMap m;
      m.setRegistry(&blockRegistry());
      for (const auto& [k, col] : w.columns()) { auto c = std::make_unique<ChunkColumn>(); c->cx = col->cx; c->cz = col->cz; m.insert(std::move(c)); }
      return m;
    };
    auto sameCells = [](const ChunkMap& a, const ChunkMap& b) {
      if (a.columns().size() != b.columns().size()) return false;
      for (const auto& [k, ca] : a.columns()) {
        const auto it = b.columns().find(k);
        if (it == b.columns().end()) return false;
        for (int sy = 0; sy < kSubChunks; ++sy) {
          const SubChunk* x = ca->subs[size_t(sy)].get(); const SubChunk* y = it->second->subs[size_t(sy)].get();
          const bool ex = !x || !x->nonAir, ey = !y || !y->nonAir;
          if (ex != ey) return false;
          // SEMANTIC equality (P2b-2, review SV-M1/C-M1): compare the
          // resolved voxel at every cell (blocks.at(i)) + its state, NOT
          // the palette representation. A save/load round trip rebuilds
          // palettes in scan order, so insertion order legitimately
          // differs for structure/edit-bearing sub-chunks while every
          // voxel is identical — the old `x->blocks != y->blocks`
          // (VoxelPalette::operator==, representation equality) failed 4
          // round-trip checks on bit-perfect data. operator== itself is
          // NOT changed: representation equality is legitimately used
          // elsewhere.
          if (!ex) {
            for (int i = 0; i < kSubChunkVolume; ++i)
              if (x->blocks.at(i) != y->blocks.at(i) || x->state[size_t(i)] != y->state[size_t(i)]) return false;
          }
        }
      }
      return true;
    };
    const auto noDirty = [](int, int, int) {};
    for (int i = 0; i < 25; ++i) sig.tick(wafer->chunks, 2, 2, 12, noDirty);  // clocks mid-cycle: live memory
    const auto stateBefore = sig.exportState();
    size_t memBytes = 0; for (const auto& [k, b] : stateBefore) memBytes += b.size();
    ChunkMap restored = emptyLike(wafer->chunks);
    std::vector<uint8_t> blob;
    int n = 0;
    {
      WorldSave save(dir);
      save.saveDimension("wafer", wafer->chunks, sig, true);
      PlayerMeta pm{{1.5f, 90.25f, -3.f}, 0.5f, -0.25f, "organic", 4, {kRedTrace, kAir, kLamp}};
      std::string err;
      const bool wrote = save.writePlayer(pm, &err);
      const auto back = save.readPlayer(&err);
      check(wrote && back && back->pos.y == 90.25f && back->dimension == "organic" && back->hot == 4 && back->hotbar == pm.hotbar,
            "save: player metadata (position, look, dimension, hotbar) round trip");
    }
    {
      WorldSave save(dir);  // a fresh process view of the same directory
      n = save.loadDimension("wafer", restored, blob);
    }
    SignalSim sig2;
    sig2.scan(restored);
    const bool imported = sig2.importState(blob);
    check(n == int(wafer->chunks.columns().size()) && imported && memBytes > 0 && sameCells(wafer->chunks, restored) &&
              sig2.exportState() == stateBefore,
          "save: blocks, cell states and component memory / timers restore bit for bit");
    check(restored.get(int(sp.x) - 3, int(sp.y), int(sp.z)) == kFormulaShrub311 &&
              restored.get(int(sp.x) + 3, int(sp.y), int(sp.z)) == kFormulaShrub314,
          "spawn garden: both shrubs survive the save/load round trip");
    for (int i = 0; i < 70; ++i) { sig.tick(wafer->chunks, 2, 2, 12, noDirty); sig2.tick(restored, 2, 2, 12, noDirty); }
    check(sameCells(wafer->chunks, restored) && sig2.exportState() == sig.exportState(),
          "save: the restored world ticks identically to the original (70 more ticks)");
    {
      std::fstream f(dir / "player.dat", std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(12); f.put('\x7f'); f.close();
      std::string err;
      WorldSave save(dir);
      check(!save.readPlayer(&err) && !err.empty(), "save: a damaged player.dat is detected (CRC), not misread");
    }
    {  // P2b-1 (Appendix G H1/H2 + Appendix C M2): player.dat v4 — the
       // canonical 42+8 inventory persists, the hotbar is stored as string
       // ids and name-remapped on load, and v1–v3 files keep loading.
      const auto pdir = dir / "p2b1_player";
      auto f32w = [](storage::ByteWriter& w, float v) { uint32_t u; std::memcpy(&u, &v, 4); w.u32(u); };
      auto writeRawPlayer = [&](const std::filesystem::path& d, const storage::Bytes& body) {
        std::filesystem::create_directories(d);
        storage::ByteWriter f;
        f.u32(0x4D504C4Cu);  // "LLPM"
        f.u32(storage::crc32(body.data(), body.size()));
        f.bytes(body);
        std::ofstream out(d / "player.dat", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(f.out.data()), std::streamsize(f.out.size()));
      };
      auto slotIs = [](const Inventory& inv, int flat, uint32_t id, uint16_t count) {
        const ItemStack& s = inv.slotAt(flat);
        return s.itemId == id && s.count == count;
      };
      {  // v4 round trip: every slot kind filled — hotbar incl. the Fragment
         // and a tool, an offhand, main, and gear — comes back identical.
        PlayerMeta pm{{2.5f, 80.f, 7.f}, 0.25f, 0.5f, "wafer", 3, {kLamp, kAir, kRedTrace}};
        pm.inventory.hotbar()[0] = ItemStack{kItemFragment, 1};
        pm.inventory.hotbar()[1] = ItemStack{uint32_t(kLamp), 33};
        pm.inventory.hotbar()[3] = ItemStack{kItemBashor, 1};
        pm.inventory.offhandLeft() = ItemStack{kItemSilicaSeed, 200};
        pm.inventory.main()[5] = ItemStack{uint32_t(kChipOR), 12};
        pm.inventory.gear()[kGearHead] = ItemStack{kItemAtlas, 1};
        pm.inventory.gear()[kGearMind] = ItemStack{uint32_t(kStone), 64};
        pm.hasInventory = true;
        std::string err;
        WorldSave save(pdir);
        const bool wrote = save.writePlayer(pm, &err);
        const auto back = save.readPlayer(&err);
        bool same = back && back->fileVersion == PlayerMeta::kVersion && back->hasInventory && back->hotbar == pm.hotbar;
        if (back)
          for (int i = 0; i < kTotalSlots; ++i) {
            const ItemStack& a = pm.inventory.slotAt(i);
            same = same && slotIs(back->inventory, i, a.itemId, a.count);
          }
        check(wrote && same,
              "save (P2b-1): player.dat (current version) round-trips the full inventory (hotbar + offhand + main + gear, Fragment + tool + block stacks)");
        // The persisted Fragment is seen by the grant check: no duplicate.
        bool noDup = false;
        if (back) {
          Inventory reloaded = back->inventory;
          noDup = reloaded.countOf(kItemFragment) == 1 && !grantStartingItems(reloaded) &&
                  reloaded.countOf(kItemFragment) == 1;
        }
        check(noDup, "save (P2b-1): a saved Fragment is kept across save/load — the boot grant does not duplicate it");
      }
      {  // v3 file (hand-built, the old layout: raw u16 hotbar, stats,
         // vitals, NO inventory block): loads with an empty inventory and
         // the old raw-id hotbar behavior.
        storage::ByteWriter w;
        w.u16(3);
        f32w(w, 1.f); f32w(w, 64.f); f32w(w, 2.f); f32w(w, 0.5f); f32w(w, -0.25f);
        w.bytes(storage::Bytes{'w', 'a', 'f', 'e', 'r'});
        w.u8(2);                       // hot slot
        w.u8(3);                       // hotbar count
        w.u16(kRedTrace); w.u16(kAir); w.u16(kLamp);
        w.u16(0);                      // stats
        w.u16(0);                      // vitals
        writeRawPlayer(pdir, w.out);
        WorldSave save(pdir);
        std::string err;
        const auto back = save.readPlayer(&err);
        check(back && back->fileVersion == 3 && !back->hasInventory && back->inventory.isEmpty() &&
                  back->hot == 2 && back->hotbar == std::vector<BlockId>({kRedTrace, kAir, kLamp}),
              "save (P2b-1): a v3 player.dat loads with an empty inventory and its raw-id hotbar intact");
      }
      {  // v4 string remap (rename simulation): the pre-rename alias
         // "lithokraft:red_wire" must follow to Red Trace in BOTH the
         // hotbar field and an inventory slot; an unknown item string
         // drops only its own stack.
        storage::ByteWriter w;
        w.u16(4);
        f32w(w, 1.f); f32w(w, 64.f); f32w(w, 2.f); f32w(w, 0.f); f32w(w, 0.f);
        w.bytes(storage::Bytes{'w', 'a', 'f', 'e', 'r'});
        w.u8(0);
        w.u8(2);  // hotbar: red_wire (alias), empty
        w.bytes(storage::Bytes{'l', 'i', 't', 'h', 'o', 'k', 'r', 'a', 'f', 't', ':', 'r', 'e', 'd', '_', 'w', 'i', 'r', 'e'});
        w.bytes(storage::Bytes{});
        w.u16(0);  // stats
        w.u16(0);  // vitals
        w.u16(uint16_t(kTotalSlots));
        auto putSlot = [&w](const char* sid, uint16_t count) {
          w.bytes(storage::Bytes(sid, sid + std::strlen(sid)));
          w.u16(count);
        };
        putSlot("lithokraft:red_wire", 7);        // slot 0: alias -> Red Trace stack
        putSlot("lithokraft:fragment", 1);        // slot 1: the Fragment
        putSlot("lithokraft:no_such_item", 5);    // slot 2: unknown -> dropped
        for (int i = 3; i < kTotalSlots; ++i) putSlot("", 0);
        writeRawPlayer(pdir, w.out);
        WorldSave save(pdir);
        std::string err;
        const auto back = save.readPlayer(&err);
        check(back && back->hasInventory && back->hotbar.size() == 2 && back->hotbar[0] == kRedTrace &&
                  back->hotbar[1] == kAir && slotIs(back->inventory, 0, uint32_t(kRedTrace), 7) &&
                  slotIs(back->inventory, 1, kItemFragment, 1) && back->inventory.slotAt(2).empty(),
              "save (P2b-1): v4 hotbar + inventory string ids name-remap on load (rename alias follows); an unknown item drops alone");
      }
      {  // C-L1 (P3-2): a v4 body with a trailing byte (CRC covers it, so
         // only the exact-consumption check can catch it) is refused; and
         // writePlayer refuses a hotbar whose count would truncate.
        storage::ByteWriter w;
        w.u16(4);
        f32w(w, 1.f); f32w(w, 64.f); f32w(w, 2.f); f32w(w, 0.f); f32w(w, 0.f);
        w.bytes(storage::Bytes{'w', 'a', 'f', 'e', 'r'});
        w.u8(0);   // hot slot
        w.u8(0);   // hotbar count
        w.u16(0);  // stats
        w.u16(0);  // vitals
        w.u16(0);  // inventory slots
        w.u8(0xEE);  // trailing garbage
        writeRawPlayer(pdir, w.out);
        WorldSave save(pdir);
        std::string err;
        check(!save.readPlayer(&err) && err.find("trailing") != std::string::npos,
              "save (C-L1): player.dat with trailing bytes after the last block is refused");
        PlayerMeta big;
        big.hotbar.assign(300, kLamp);
        std::string werr;
        check(!save.writePlayer(big, &werr) && !werr.empty(),
              "save (C-L1): writePlayer refuses a 300-slot hotbar instead of truncating its u8 count");
      }
    }
    {
      // Worldgen determinism: same seed -> identical columns at any thread count and on a fresh load.
      auto genWith = [&](unsigned threads) {
        JobPool p(threads);
        HandoffQueue<std::unique_ptr<ChunkColumn>> out;
        {
          JobBatch b(p);
          for (int z = 0; z < 4; ++z)
            for (int x = 0; x < 4; ++x) b.submit([&, x, z] { out.push(wafer->generator->generate(x + 300, z - 2)); });
        }
        std::vector<std::unique_ptr<ChunkColumn>> cols;
        out.drainInto(cols);
        ChunkMap m;
        m.setRegistry(&blockRegistry());
        for (auto& c : cols) if (c) m.insert(std::move(c));
        return m;
      };
      const ChunkMap g1 = genWith(1), g4 = genWith(4), gAll = genWith(std::max(2u, std::thread::hardware_concurrency()));
      check(g1.columns().size() == 16 && sameCells(g1, g4) && sameCells(g1, gAll),
            "worldgen: same seed gives identical columns with 1, 4 and all threads");
      DimensionRegistry r1 = createDimensions(), r2 = createDimensions();
      Dimension* a = r1.find("wafer"); Dimension* b = r2.find("wafer");
      a->load(pool, 6);
      b->load(pool, 6);
      check(sameCells(a->chunks, b->chunks), "worldgen: two fresh loads of a dimension (terrain + structures) are identical");

      // Partial saves: save (nothing changed: only component columns), edit, save the changed
      // column, edit + tick, save again; a fresh load (regenerate + overlay the save) must match.
      const auto pdir = dir / "partial";
      SignalSim sa;
      sa.scan(a->chunks);
      std::unordered_set<uint64_t> changed;
      size_t written = 0;
      {
        WorldSave save(pdir);
        written += save.saveDimension("wafer", a->chunks, sa, true, &changed);
        a->chunks.set(5, 100, 5, kLamp);       // edit column (0,0)
        a->chunks.set(70, 90, 40, kRedTrace);   // edit column (4,2)
        changed = {ChunkKey::make(0, 0), ChunkKey::make(4, 2)};
        written += save.saveDimension("wafer", a->chunks, sa, true, &changed);
        a->chunks.set(5, 100, 5, kAir);        // undo in (0,0), new edit in (1,3)
        a->chunks.set(20, 95, 50, kLamp);
        changed = {ChunkKey::make(0, 0), ChunkKey::make(1, 3)};
        // Signal ticks change wire / lamp states: their columns get marked, as SimThread does.
        const auto mark = [&](int x, int, int z) { changed.insert(ChunkKey::make(x >> 4, z >> 4)); };
        for (int i = 0; i < 30; ++i) sa.tick(a->chunks, 2, 2, 12, mark);
        written += save.saveDimension("wafer", a->chunks, sa, true, &changed);
      }
      DimensionRegistry r3 = createDimensions();
      Dimension* c = r3.find("wafer");
      c->load(pool, 6);  // regenerated: untouched columns come from the seed
      std::vector<uint8_t> state;
      { WorldSave save(pdir); save.loadDimension("wafer", c->chunks, state); }
      SignalSim sc;
      sc.scan(c->chunks);
      const bool imp = sc.importState(state);
      check(written < 3 * a->chunks.columns().size() && imp && sameCells(a->chunks, c->chunks) && sc.exportState() == sa.exportState(),
            "save: save / edit / save / edit+tick / save, then regenerate + load matches bit for bit (cells, states, component memory)");
      for (int i = 0; i < 40; ++i) { sa.tick(a->chunks, 2, 2, 12, noDirty); sc.tick(c->chunks, 2, 2, 12, noDirty); }
      check(sameCells(a->chunks, c->chunks) && sc.exportState() == sa.exportState(), "save: the partially saved world ticks identically after load");
      // Namespaced ids: unique, and a pre-table (legacy) save still loads as the current order.
      std::unordered_set<std::string> ids;
      bool namespaced = true;
      for (size_t i = 1; i < blockRegistry().size(); ++i) {
        const std::string id = blockRegistry().idOf(BlockId(i));
        namespaced = namespaced && id.rfind("lithokraft:", 0) == 0 && ids.insert(id).second;
      }
      // Frozen ids: saves depend on these strings; they are explicit, not derived from textures.
      const bool frozen = blockRegistry().idOf(kWaferSilicon) == "lithokraft:silicon" && blockRegistry().idOf(kRedTorch) == "lithokraft:red_torch" &&
                          blockRegistry().idOf(kSwitch) == "lithokraft:switch" && blockRegistry().idOf(kButton) == "lithokraft:button" &&
                          blockRegistry().idOf(kLamp) == "lithokraft:lamp";
      check(frozen, "blocks: save ids are explicit and frozen (silicon, red_torch, switch, button, lamp)");
      const bool table = std::filesystem::exists(pdir / "blocks.names");
      std::filesystem::remove(pdir / "blocks.names");
      DimensionRegistry r4 = createDimensions();
      Dimension* l = r4.find("wafer");
      l->load(pool, 6);
      std::vector<uint8_t> st2;
      { WorldSave save(pdir); save.loadDimension("wafer", l->chunks, st2); }
      check(namespaced && table && sameCells(c->chunks, l->chunks),
            "save: block ids are unique 'lithokraft:' names, saves write the name table, and a legacy save (no table) migrates as the current order");
    }
    {  // P0b permanence pack (C-4 / SV-H1 / SV-H2 / SV-H3): the file contract
       // applied to blocks.names + player.dat, fail-closed loads/saves, the
       // persisted seed, and honest flush-failure reporting.
      auto corruptByte = [](const std::filesystem::path& p, std::streamoff at) {
        std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(at); char ch = 0; f.get(ch); f.seekp(at); f.put(char(ch ^ 0x5A)); f.close();
      };
      auto writeGarbage = [](const std::filesystem::path& p) {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << "TORN-TABLE-GARBAGE-0123456789abcdef";
        f.close();
      };
      std::vector<uint8_t> blobP;
      {  // C-4 recovery: torn primary + intact backup generation -> table heals, columns load.
        const auto ndir = dir / "p0b_names_recovery";
        {
          WorldSave save(ndir);
          save.saveDimension("wafer", wafer->chunks, sig, true);
          save.saveDimension("wafer", wafer->chunks, sig, true);  // second write rotates blocks.names.bak.1
        }
        const bool haveBackup = std::filesystem::exists(ndir / "blocks.names.bak.1");
        writeGarbage(ndir / "blocks.names");
        ChunkMap rec = emptyLike(wafer->chunks);
        int rn = 0;
        { WorldSave save(ndir); rn = save.loadDimension("wafer", rec, blobP); }
        check(haveBackup && rn == int(wafer->chunks.columns().size()),
              "save (C-4): a torn blocks.names primary heals from the intact backup generation; every column loads");
      }
      {  // C-4 fail closed: torn table, NO intact generation, regions present.
        const auto fdir = dir / "p0b_names_failclosed";
        { WorldSave save(fdir); save.saveDimension("wafer", wafer->chunks, sig, true); }  // one save: no table backup yet
        writeGarbage(fdir / "blocks.names");
        ChunkMap fc = emptyLike(wafer->chunks);
        int fn = 0;
        bool saveRefused = false;
        {
          WorldSave save(fdir);
          fn = save.loadDimension("wafer", fc, blobP);
          saveRefused = save.saveDimension("wafer", wafer->chunks, sig, true) == 0 && !save.lastSaveOk();
        }
        std::ifstream tf(fdir / "blocks.names", std::ios::binary);
        const std::string tableAfter((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
        check(fn < 0 && saveRefused && tableAfter.rfind("TORN-TABLE-GARBAGE", 0) == 0,
              "save (C-4): a corrupt blocks.names with no intact generation + regions present fails closed (load refuses, save refuses, table untouched)");
      }
      {  // SV-H3: torn player.dat primary heals from the backup generation.
        const auto pdir2 = dir / "p0b_player";
        PlayerMeta pa{{1.5f, 111.f, -3.f}, 0.5f, -0.25f, "wafer", 1, {kLamp}};
        {
          WorldSave save(pdir2);
          std::string err;
          save.writePlayer(pa, &err);
          PlayerMeta pb = pa; pb.pos.y = 222.f;
          save.writePlayer(pb, &err);  // rotates pa into player.dat.bak.1
        }
        corruptByte(pdir2 / "player.dat", 20);
        WorldSave save(pdir2);
        std::string err;
        const auto back = save.readPlayer(&err);
        check(back && back->pos.y == 111.f, "save (SV-H3): a torn player.dat primary heals from the backup generation");
      }
      {  // SV-H2: the seed round-trips through level.json, exactly, as uint64.
        const auto sdir = dir / "p0b_seed";
        const uint64_t kSeed = 0xFFFFFFFFFFFFFFD5ull;  // > 2^53: a JSON double could not hold this exactly
        { WorldSave save(sdir); save.setSeed(kSeed); save.saveDimension("wafer", wafer->chunks, sig, true); }
        std::string serr;
        const auto seedBack = WorldSave::readWorldSeed(sdir, &serr);
        check(std::filesystem::exists(sdir / "level.json") && seedBack && *seedBack == kSeed,
              "save (SV-H2): the world seed persists in level.json and reads back exactly (uint64) with no CLI seed");
      }
      {  // SV-H1: a failed region flush surfaces as failure, and the pending
         // records kept by RegionFile land on the retry. Injection: a
         // DIRECTORY at the region file's .tmp path makes writeFileAtomic
         // fail deterministically (even running as root, where chmod-based
         // read-only directories would not).
        const auto wdir = dir / "p0b_flush";
        bool firstOk = false, failedSave = false, retryOk = false;
        {
          WorldSave save(wdir);
          save.saveDimension("wafer", wafer->chunks, sig, true);
          firstOk = save.lastSaveOk();
          std::filesystem::create_directories(wdir / "wafer" / "r.0.0.llr.tmp");
          save.saveDimension("wafer", wafer->chunks, sig, true);
          failedSave = !save.lastSaveOk();
          std::filesystem::remove_all(wdir / "wafer" / "r.0.0.llr.tmp");
          retryOk = save.flush();
        }
        ChunkMap after = emptyLike(wafer->chunks);
        int loadedAfter = 0;
        { WorldSave save(wdir); loadedAfter = save.loadDimension("wafer", after, blobP); }
        check(firstOk && failedSave && retryOk && loadedAfter == int(wafer->chunks.columns().size()),
              "save (SV-H1): a failed region flush surfaces as failure (never 'on disk') and the kept pending records land on retry");
      }
    }
    {  // P1-SIGNAL gate (SW-H1 / SW-H2 / B-M3 / C-M4 / B-L7): a circuit built
       // in a column OUTSIDE the initial load square, saved, and reloaded
       // through the streaming seam — WorldSave::loadColumn + SignalSim::
       // scanColumn/importState, the exact calls Game::pumpStreaming makes on
       // insertion, plus a ChunkStreamer pass through the real job pool —
       // must come back alive with its component memory intact. (The full
       // Game-level loop is NOT driven here: no Game/SimThread is constructed
       // in this test, so mesh submission and the stop/start seam itself are
       // covered only by the in-game path, not by this check.)
      const auto pdirS = dir / "p1_stream";
      ChunkMap src;
      src.setRegistry(&blockRegistry());
      for (int cc : {0, 20}) {
        auto c = std::make_unique<ChunkColumn>(); c->cx = cc; c->cz = cc; src.insert(std::move(c));
      }
      const int gy = 100;  // circuit plane in column (20,20): x,z in [320,336)
      for (int z = 320; z < 336; ++z) for (int x = 320; x < 336; ++x) src.set(x, gy - 1, z, kWaferSilicon);
      src.set(322, gy, 322, kRedTorch);
      src.set(323, gy, 322, kRedTrace); src.set(324, gy, 322, kRedTrace); src.set(325, gy, 322, kRedTrace);
      src.set(326, gy, 322, kMemor); src.setState(326, gy, 322, makeState(0, 1));  // facing E: back reads the wire run, front drives on
      src.set(327, gy, 322, kRedTrace);
      src.set(328, gy, 322, kLamp);
      SignalSim srcSim;
      srcSim.scan(src);
      for (int i = 0; i < 6; ++i) srcSim.tick(src, 20, 20, 4, noDirty);
      const auto srcState = srcSim.exportState();
      check(stValue(src.getState(326, gy, 322)) == 62 && src.getState(328, gy, 322) == 1 && !srcState.empty(),
            "p1-stream: source circuit lives before save (memor stored the decayed 62, lamp lit)");
      { WorldSave save(pdirS); save.saveDimension("wafer", src, srcSim, true); }
      // --- the streaming seam, exactly as pumpStreaming drives it ---
      WorldSave streamSave(pdirS);
      auto loaded = streamSave.loadColumn("wafer", 20, 20);
      check(loaded.status == WorldSave::ColumnLoad::Loaded && loaded.column && !loaded.signal.empty(),
            "p1-stream (SW-H1/C-M4): loadColumn returns the column AND its per-column SGNL payload");
      const auto neverSaved = streamSave.loadColumn("wafer", 21, 21);
      check(neverSaved.status == WorldSave::ColumnLoad::NotSaved && !neverSaved.column,
            "p1-stream (B-M3): a never-saved column reports NotSaved (generate fresh), distinct from unreadable");
      ChunkMap dst;
      dst.setRegistry(&blockRegistry());
      dst.insert(std::move(loaded.column));
      SignalSim dstSim;
      dstSim.scanColumn(dst, 20, 20);
      const bool impOk = dstSim.importState(loaded.signal);
      check(impOk && dstSim.exportState() == srcState,
            "p1-stream (SW-H1): scanColumn + importState restores the streamed column's component memory bit for bit");
      for (int i = 0; i < 6; ++i) dstSim.tick(dst, 20, 20, 4, noDirty);
      check(dst.getState(328, gy, 322) == 1 && stValue(dst.getState(326, gy, 322)) == 62,
            "p1-stream (SW-H1): the streamed circuit is ALIVE after reload (lamp lit, memor still holds 62)");
      {  // Dimension path on the SAME WorldSave instance, after streamed loads.
        ChunkMap dimMap = emptyLike(src);
        std::vector<uint8_t> blob;
        const int dn = streamSave.loadDimension("wafer", dimMap, blob);
        SignalSim dimSim;
        dimSim.scan(dimMap);
        check(dn == 2 && dimSim.importState(blob) && dimSim.exportState() == srcState,
              "p1-stream (C-M4): loadDimension on the same instance after streamed loads returns exactly the dimension's state (no residue, no cross-path sweep)");
      }
      // SW-H2: a hand-edited wire value of 100 in the streamed column clamps
      // TO 64 at the component input — the memor stores 64, never 100, and no
      // out-of-range output is produced (the pre-fix code indexed buckets_[100]).
      dst.setState(325, gy, 322, CellState(100));
      dstSim.invalidateNetwork();
      dstSim.tick(dst, 20, 20, 4, noDirty);
      check(stValue(dst.getState(326, gy, 322)) == 64,
            "p1-stream (SW-H2): a disk-loaded wire value of 100 is clamped TO 64 at the component input (memor stores 64)");
      {  // B-M3: corrupt name table => Unreadable per column on every call
         // (fail closed, no session latch, never mistaken for "never saved").
        const auto tdirS = dir / "p1_stream_corrupt";
        { WorldSave save(tdirS); save.saveDimension("wafer", src, srcSim, true); }  // one save: no table backup yet
        { std::ofstream f(tdirS / "blocks.names", std::ios::binary | std::ios::trunc); f << "TORN-TABLE-GARBAGE-0123456789abcdef"; }
        WorldSave corruptSave(tdirS);
        const auto bad1 = corruptSave.loadColumn("wafer", 20, 20);
        const auto bad2 = corruptSave.loadColumn("wafer", 20, 20);
        check(bad1.status == WorldSave::ColumnLoad::Unreadable && bad2.status == WorldSave::ColumnLoad::Unreadable && !bad1.column,
              "p1-stream (B-M3): corrupt name table => Unreadable on every call (fail closed per column, no latch, never 'never saved')");
        // ChunkStreamer integration: the healthy path delivers disk + SGNL
        // through the real job pool (SW-H3 guard on its normal exit), and an
        // unreadable column is neither delivered nor generated over, with
        // its key un-requested for a backed-off retry (B-L7).
        ChunkStreamer good(pool, wafer->generator, &streamSave, "wafer", 0, {});
        good.update(20, 20);
        bool got = false;
        for (int i = 0; i < 500 && !got; ++i) { got = good.hasReady(); if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        auto readyCols = good.takeReady();
        check(got && readyCols.size() == 1 && readyCols[0].fromDisk && readyCols[0].column && readyCols[0].column->cx == 20 && !readyCols[0].signal.empty(),
              "p1-stream (SW-H1/H3): ChunkStreamer delivers the disk column with its SGNL payload through the job pool");
        ChunkStreamer bad(pool, wafer->generator, &corruptSave, "wafer", 0, {});
        bad.update(20, 20);
        bool erased = false;
        for (int i = 0; i < 500 && !erased; ++i) {
          bad.update(20, 20);
          erased = bad.streamedCount() == 0 && !bad.hasReady();
          if (!erased) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(erased, "p1-stream (B-M3/B-L7): an unreadable streamed column is not delivered, not generated over, and its key is un-requested for a backed-off retry");
      }
      {  // B-M2 (P3-2): takeReadyUpTo bounds every drain — a burst of
         // finished columns can no longer land in one insertion window.
        ChunkStreamer many(pool, wafer->generator, nullptr, "wafer", 1, {});
        many.update(40, 40);  // 3x3 around chunk (40,40), generate-only
        size_t total = 0;
        bool bounded = true;
        for (int i = 0; i < 1000 && total < 9; ++i) {
          auto batch = many.takeReadyUpTo(3);
          if (batch.size() > 3) bounded = false;
          total += batch.size();
          if (batch.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(bounded && total == 9,
              "stream (B-M2): takeReadyUpTo(3) never returns more than 3, and all 9 requested columns arrive across successive pumps");
      }
    }
    std::filesystem::remove_all(dir);
  }
  {  // P3-2 (World/save tail): B-L2 packPos range, B-L5 LightEngine clamp,
     // B-M4 trace-strength keys, C-M3 region size cap, C-L2 .torn rotation,
     // C-L3 AnvilRegion quarantine count.
    using namespace litholattice;
    // B-L2: packPos round-trips inside its range and ALIASES outside it
    // (the alias pair is the documented hazard the bounds protect against).
    int rx = 0, ry = 0, rz = 0;
    unpackPos(packPos(-300000, 767, 262143), rx, ry, rz);
    check(packPosInRange(-300000, 767, 262143) && rx == -300000 && ry == 767 && rz == 262143 &&
              !packPosInRange(0, 0, 1 << 20) && packPos(0, 0, 1 << 20) == packPos(1, 0, -(1 << 20)),
          "packPos (B-L2): in-range coordinates round-trip exactly; z = +2^20 is out of range and aliases onto (x=1, z=-2^20)");
    // B-L5: an over-range source level clamps to kMaxLight.
    LightEngine le(16, 16, 16);
    le.addSource(8, 8, 8, 200);
    le.propagate();
    check(le.getLight(8, 8, 8) == kMaxLight, "light (B-L5): addSource level 200 clamps to kMaxLight (64)");
    // B-M4: strength slots are keyed by WORLD packPos end to end, and
    // "no slot" is a sentinel distinct from the real slot 0.
    ChunkMap tm;
    tm.setRegistry(&blockRegistry());
    { auto c = std::make_unique<ChunkColumn>(); c->cx = 3; c->cz = 3; tm.insert(std::move(c)); }
    for (int z = 48; z < 64; ++z) for (int x = 48; x < 64; ++x) tm.set(x, 99, z, kWaferSilicon);
    tm.set(50, 100, 50, kRedTorch);
    tm.set(51, 100, 50, kRedTrace);
    tm.set(52, 100, 50, kRedTrace);
    tm.set(53, 100, 50, kLamp);
    SignalSim ts;
    ts.scan(tm);
    for (int i = 0; i < 6; ++i) ts.tick(tm, 3, 3, 4, [](int, int, int) {});
    auto& tbuf = ts.strengthBuffer();
    const int64_t wireKey = packPos(51, 100, 50);
    const int wireVal = stValue(tm.getState(51, 100, 50));
    check(wireVal > 0 && tbuf.slotOf(wireKey) != TraceStrengthBuffer::kNoSlot &&
              tbuf.get(tbuf.slotOf(wireKey)) == wireVal,
          "trace strengths (B-M4): the full pass records the wire's final strength under its WORLD packPos key");
    check(TraceStrengthBuffer::kNoSlot != 0 && tbuf.slotOf(packPos(67, 100, 50)) == TraceStrengthBuffer::kNoSlot,
          "trace strengths (B-M4): the same local coords in another chunk have NO slot (world keying), and kNoSlot != real slot 0");
    const auto pdir = uniqueTempDir("lithokraft_p32_selftest");
    std::filesystem::create_directories(pdir);
    {  // C-M3: a region file past the 64 MiB cap is refused at open.
      const auto rp = pdir / "r.0.0.llr";
      { storage::RegionFile rf(rp); std::string e; rf.open(&e); rf.put(0, 0, storage::Bytes{1, 2, 3}); rf.flush(&e); }
      std::filesystem::resize_file(rp, storage::RegionFile::kMaxFileBytes + 1);
      storage::RegionFile rf2(rp);
      std::string e2;
      check(!rf2.open(&e2) && e2.find("cap") != std::string::npos,
            "storage (C-M3): a region file over the 64 MiB cap is refused at open, before any whole-file read");
    }
    {  // C-L2: recovering twice never destroys the first .torn evidence.
      const auto tp = pdir / "torn.bin";
      auto validate = [](const std::filesystem::path& q, std::string* e) {
        storage::Bytes b;
        return storage::readFileAll(q, &b, e) && b.size() == 1;
      };
      auto readAll = [&](const std::filesystem::path& q) { storage::Bytes b; std::string e; storage::readFileAll(q, &b, &e); return b; };
      std::string e;
      storage::writeFileAtomic(tp, storage::Bytes{1}, &e);
      storage::writeFileAtomic(tp, storage::Bytes{2}, &e);  // .bak.1 = {1}
      { std::ofstream f(tp, std::ios::binary | std::ios::trunc); f << "GG"; }
      const bool r1 = storage::recoverCrashedWrite(tp, validate, &e);
      { std::ofstream f(tp, std::ios::binary | std::ios::trunc); f << "HH"; }
      const bool r2 = storage::recoverCrashedWrite(tp, validate, &e);
      check(r1 && r2 && readAll(tp) == storage::Bytes{1} && readAll(tp.string() + ".torn") == storage::Bytes{'G', 'G'} &&
                readAll(tp.string() + ".torn.1") == storage::Bytes{'H', 'H'},
            "storage (C-L2): two recoveries keep BOTH torn primaries (.torn and .torn.1); the first evidence is never overwritten");
    }
    {  // C-L3: a malformed AnvilRegion index entry is quarantined + counted.
      const auto ap = pdir / "r.0.0.llar";
      size_t before = 0;
      { storage::AnvilRegion ar(ap, 0, 0); std::string e; ar.open(&e); ar.put(1, 1, storage::AnvilRegion::kCodecNone, storage::Bytes{7, 7, 7}, &e); before = ar.count(); }
      {  // entry 7 (lx=7, lz=0): nonzero offset with a zero count = malformed
        std::fstream f(ap, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(16 + 4 * 7);
        const char bad[4] = {0, 0, 5, 0};
        f.write(bad, 4);
      }
      storage::AnvilRegion ar2(ap, 0, 0);
      std::string e;
      check(ar2.open(&e) && ar2.quarantinedEntryCount() == 1 && ar2.count() == before,
            "storage (C-L3): a malformed AnvilRegion index entry is normalised to absent, logged and counted (not silent)");
    }
    std::filesystem::remove_all(pdir);
  }
  {  // A2 (B-M1 column eviction, 2026-10-13): the production seams composed
     // exactly as Game::evictFarColumns drives them — SGNL export before
     // unscan, save-on-evict through WorldSave::saveColumns, ChunkMap::erase,
     // ChunkStreamer::forgetColumn, and selectEvictionCandidates bounding.
     // (The full Game loop is not constructed here; mesh retire is a
     // renderer-queue operation covered by the empty-verts contract.)
    using namespace litholattice;
    const auto evNoDirty = [](int, int, int) {};
    const auto edir = uniqueTempDir("lithokraft_a2_evict_selftest");
    std::filesystem::remove_all(edir);
    ChunkMap ew;
    ew.setRegistry(&blockRegistry());
    for (int cc : {20, 21}) {
      auto c = std::make_unique<ChunkColumn>(); c->cx = cc; c->cz = 20; ew.insert(std::move(c));
    }
    check(!ew.column(20, 20)->dirty && ew.column(20, 20)->generation == 0,
          "evict (A2): a freshly inserted column starts clean (dirty=false, generation=0)");
    const int gy = 100;
    for (int z = 320; z < 336; ++z) for (int x = 320; x < 336; ++x) ew.set(x, gy - 1, z, kWaferSilicon);
    ew.set(322, gy, 322, kRedTorch);
    ew.set(323, gy, 322, kRedTrace); ew.set(324, gy, 322, kRedTrace);
    ew.set(325, gy, 322, kMemor); ew.setState(325, gy, 322, makeState(0, 1));
    ew.set(326, gy, 322, kRedTrace);
    ew.set(327, gy, 322, kLamp);
    // A plain neighbour column in (21,20): terrain only, then left clean by
    // direct column construction (mutations below bypass the map on purpose:
    // worldgen writes columns directly, never through ChunkMap::set).
    {
      ChunkColumn plain; plain.cx = 21; plain.cz = 20;
      // Rebuild via the map so the column exists, then reset its flags to the
      // worldgen state (clean) — the fixture's terrain is seed-equivalent.
      ew.set(340, gy - 1, 322, kWaferSilicon);
      ew.column(21, 20)->dirty = false; ew.column(21, 20)->generation = 0;
    }
    check(ew.column(20, 20)->dirty && ew.column(20, 20)->generation > 0,
          "evict (A2): ChunkMap::set marks the resident column dirty and bumps its generation");
    SignalSim esim;
    esim.scan(ew);
    for (int i = 0; i < 6; ++i) esim.tick(ew, 20, 20, 4, evNoDirty);
    check(esim.hasIndexedCells(20, 20) && !esim.exportColumnState(20, 20).empty(),
          "evict (A2): the circuit column is sim-indexed and exports per-column SGNL state");
    const uint64_t hashBefore = hashControlWorld(ew, esim, 0);
    const auto stateBefore = esim.exportState();
    // --- dirty eviction: save-on-evict, then unscan + erase ---------------
    {
      WorldSave save(edir);
      const size_t saved = save.saveColumns("wafer", {ew.column(20, 20)}, esim, true);
      check(saved == 1 && save.lastSaveOk(),
            "evict (A2): saveColumns persists a dirty column (blocks + SGNL) before eviction");
    }
    esim.unscanColumn(20, 20);
    check(!esim.hasIndexedCells(20, 20) && esim.exportState().empty(),
          "evict (A2): unscanColumn leaves NO ghost components (export is empty for the evicted column)");
    check(ew.erase(20, 20) && !ew.contains(20, 20) && !ew.erase(20, 20),
          "evict (A2): ChunkMap::erase removes the column exactly once");
    for (int i = 0; i < 6; ++i) esim.tick(ew, 20, 20, 4, evNoDirty);
    check(esim.exportState().empty() && esim.componentCount() == 0,
          "evict (A2): ticking after eviction resurrects nothing (no ghost sim state)");
    // --- reload: the evicted column comes back bit-exact ------------------
    ChunkMap reloaded;
    reloaded.setRegistry(&blockRegistry());
    {
      auto c = std::make_unique<ChunkColumn>(); c->cx = 21; c->cz = 20; reloaded.insert(std::move(c));
      // Column (21,20) reloads as regenerated terrain (clean eviction path:
      // never saved; the fixture terrain is reproduced directly).
      reloaded.set(340, gy - 1, 322, kWaferSilicon);
      reloaded.column(21, 20)->dirty = false; reloaded.column(21, 20)->generation = 0;
    }
    WorldSave reloadSave(edir);
    auto loadedCol = reloadSave.loadColumn("wafer", 20, 20);
    check(loadedCol.status == WorldSave::ColumnLoad::Loaded && loadedCol.column && !loadedCol.signal.empty(),
          "evict (A2): loadColumn returns the evicted column with its SGNL payload");
    reloaded.insert(std::move(loadedCol.column));
    // Production scans per column on insertion; import the evicted
    // column's payload after its scanColumn, as pumpStreaming does.
    SignalSim rsim2;
    rsim2.scanColumn(reloaded, 20, 20);
    const bool impOk = rsim2.importState(loadedCol.signal);
    check(impOk && rsim2.exportState() == stateBefore,
          "evict (A2): generate->mutate->evict->reload restores SGNL state bit for bit");
    check(hashControlWorld(reloaded, rsim2, 0) == hashBefore,
          "evict (A2): generate->mutate->evict->reload produces a bit-exact world hash (blocks + states + SGNL)");
    // --- clean eviction: no save needed, reload equality ------------------
    ChunkMap cleanMap;
    cleanMap.setRegistry(&blockRegistry());
    { auto c = std::make_unique<ChunkColumn>(); c->cx = 30; c->cz = 30; cleanMap.insert(std::move(c)); }
    cleanMap.set(481, 90, 481, kStone);  // fixture edit, then restore clean flags (worldgen-equivalent)
    cleanMap.column(30, 30)->dirty = false; cleanMap.column(30, 30)->generation = 0;
    SignalSim csim;
    csim.scan(cleanMap);
    const uint64_t cleanHash = hashControlWorld(cleanMap, csim, 0);
    check(!cleanMap.column(30, 30)->dirty && !csim.hasIndexedCells(30, 30) && cleanMap.erase(30, 30),
          "evict (A2): a clean, signal-free column evicts with no save (fast path)");
    ChunkMap cleanBack;
    cleanBack.setRegistry(&blockRegistry());
    { auto c = std::make_unique<ChunkColumn>(); c->cx = 30; c->cz = 30; cleanBack.insert(std::move(c)); }
    cleanBack.set(481, 90, 481, kStone);
    cleanBack.column(30, 30)->dirty = false; cleanBack.column(30, 30)->generation = 0;
    SignalSim csim2;
    csim2.scan(cleanBack);
    check(hashControlWorld(cleanBack, csim2, 0) == cleanHash,
          "evict (A2): clean column evict/reload equality (regenerated column hashes identically)");
    // --- bounded residency: candidate selection + forget + erase ----------
    {
      std::vector<uint64_t> keys;
      ChunkMap big;
      big.setRegistry(&blockRegistry());
      for (int cz = -20; cz <= 20; ++cz) for (int cx = -20; cx <= 20; ++cx) {
        auto c = std::make_unique<ChunkColumn>(); c->cx = cx; c->cz = cz; big.insert(std::move(c));
        keys.push_back(ChunkKey::make(cx, cz));
      }
      // Player at (0,0), stream radius 8, hysteresis +2 (JD-DECISION values):
      // resident count must converge to the (2*10+1)^2 = 441 in-band columns.
      size_t guard = 0, maxBatch = 0;
      while (big.columns().size() > 441 && guard++ < 500) {
        std::vector<uint64_t> cur;
        for (const auto& [k, col] : big.columns()) cur.push_back(k);
        const auto cands = selectEvictionCandidates(cur, 0, 0, 8, 2, 4);
        if (cands.empty()) break;
        maxBatch = std::max(maxBatch, cands.size());
        for (uint64_t k : cands) big.erase(int(int32_t(k >> 32)), int(int32_t(k & 0xffffffffu)));
      }
      check(maxBatch <= 4, "evict (A2): eviction is bounded per pump (budget 4)");
      check(big.columns().size() == 441,
            "evict (A2): resident column count stays bounded (1681 explored -> 441 within radius+hysteresis)");
      const auto none = selectEvictionCandidates(keys, 0, 0, 100, 2, 4);
      check(none.empty(), "evict (A2): no candidates inside the hysteresis band (no boundary thrash)");
      // forgetColumn makes an evicted column requestable again.
      ChunkStreamer fs(pool, wafer->generator, nullptr, "wafer", 0, {ChunkKey::make(50, 50)});
      fs.forgetColumn(50, 50);
      check(fs.requestedCount() == 0, "evict (A2): ChunkStreamer::forgetColumn removes the requested_ entry (re-streamable)");
    }
    std::filesystem::remove_all(edir);
  }
  {  // A3 (brief B1/B1a no-decision subset, 2026-10-13): the per-item use
     // registry dispatches defined handlers and fails closed for the rest.
     // (FragmentState production ownership + the markdown flag path are
     // proven against a real Game in the headless harness suite.)
    using namespace litholattice;
    ChunkMap iw;
    iw.setRegistry(&blockRegistry());
    { auto c = std::make_unique<ChunkColumn>(); c->cx = 0; c->cz = 0; iw.insert(std::move(c)); }
    iw.set(5, 64, 5, kWaferSilicon);
    iw.set(7, 64, 5, kDeadSilicon);
    const IWorldSimulation::DirtyFn noMark = [](int, int, int) {};
    check(hasItemUseHandler(kItemSeedor) && hasItemUseHandler(kItemSilicaSeed) && hasItemUseHandler(kItemFragment) &&
          !hasItemUseHandler(kItemBashor) && !hasItemUseHandler(999999),
          "item-use (A3/B1a): handlers registered for defined items only (SeedOR, silica seed, Fragment stub)");
    check(dispatchItemUse(kItemSeedor, ItemUseContext{iw, 5, 64, 5, noMark}) == ItemUseResult::Handled &&
          iw.get(5, 64, 5) == kSilicaTilledSoil,
          "item-use (A3/B1a): SeedOR dispatch tills silicon soil (ToolAction::Till behavior)");
    check(dispatchItemUse(kItemSeedor, ItemUseContext{iw, 7, 64, 5, noMark}) == ItemUseResult::Failed &&
          iw.get(7, 64, 5) == kDeadSilicon,
          "item-use (A3/B1a): SeedOR on dead silicon fails (defined refusal), world unchanged");
    check(dispatchItemUse(kItemSilicaSeed, ItemUseContext{iw, 5, 64, 5, noMark}) == ItemUseResult::Handled &&
          iw.get(5, 65, 5) == kSilicaGrass,
          "item-use (A3/B1a): silica seed dispatch plants on tilled soil");
    check(dispatchItemUse(kItemFragment, ItemUseContext{iw, 5, 64, 5, noMark}) == ItemUseResult::Failed,
          "item-use (A3/B1a): the Fragment's use fails closed as decision-pending (JD C3 open verb)");
    check(dispatchItemUse(kItemBashor, ItemUseContext{iw, 5, 64, 5, noMark}) == ItemUseResult::Failed &&
          dispatchItemUse(999999, ItemUseContext{iw, 5, 64, 5, noMark}) == ItemUseResult::Failed,
          "item-use (A3/B1a): undefined items fail closed (logged), never fake success");
  }
  {  // A5 (projectile density plumbing, 2026-10-13): BlockDef::density now
     // flows from the content table into engine projectile behavior through
     // the exact seam Game wires in production (registry solidity+density).
    using namespace litholattice;
    const BlockRegistry& reg5 = blockRegistry();
    check(reg5.def(kDirt).density == 20 && reg5.def(kStone).density == 50 && reg5.def(kCeramic).density == 80 &&
          reg5.def(kGlass).density == 10 && reg5.def(kChipOR).density == 70 && reg5.def(kRedTrace).density == 0,
          "density (A5): material-class table values land in BlockDef (earth 20, stone 50, ceramic 80, glass 10, ChipOR 70)");
    bool coverage = true;
    for (size_t i = 1; i < reg5.size(); ++i) {
      const BlockDef& d = reg5.def(BlockId(i));
      if (reg5.has(BlockId(i), kSolid) == (d.density == 0)) coverage = false;  // solid <=> density > 0
    }
    check(coverage, "density (A5): every solid block carries density > 0; every non-solid stays 0 (never consulted)");
    ChunkMap pw;
    pw.setRegistry(&reg5);
    { auto c = std::make_unique<ChunkColumn>(); c->cx = 0; c->cz = 0; pw.insert(std::move(c)); }
    ProjectileManager::VoxelQueryFn q5 = [&pw, &reg5](int x, int y, int z) {
      ProjectileManager::VoxelInfo v;
      const BlockId b = pw.get(x, y, z);
      v.solid = reg5.has(b, kSolid);
      v.density = reg5.def(b).density;
      return v;
    };
    auto shoot = [&](BlockId wall, uint8_t pen, double& maxX, bool& alive) {
      for (int x = 10; x <= 12; ++x) pw.set(x, 100, 0, wall);
      ProjectileManager pm;
      pm.setVoxelQuery(q5);
      Projectile p;
      p.x = 0; p.y = 100.5; p.z = 0.5; p.vx = 2; p.penetration = pen; p.maxRange = 200;
      const uint64_t id = pm.spawn(p, nullptr);
      maxX = 0; alive = true;
      for (int i = 0; i < 40 && pm.alive(id); ++i) { pm.tick(); if (const Projectile* pr = pm.get(id)) maxX = std::max(maxX, pr->x); }
      alive = pm.alive(id);
      for (int x = 10; x <= 12; ++x) pw.set(x, 100, 0, kAir);
    };
    double mx; bool al;
    shoot(kDirt, 30, mx, al);
    check(al && mx > 13, "density (A5): penetration 30 pierces a dirt wall (density 20) — density flows from block data");
    shoot(kDirt, 10, mx, al);
    check(!al && mx < 10, "density (A5): penetration 10 stops and despawns at a dirt wall (density 20)");
    shoot(kCeramic, 30, mx, al);
    check(!al && mx < 10, "density (A5): penetration 30 stops at a ceramic wall (density 80) that dirt let through");
    shoot(kCeramic, 90, mx, al);
    check(al && mx > 13, "density (A5): penetration 90 pierces ceramic (density 80) — the pierce rule discriminates by data");
  }
  const BlockMaterials checker(blockRegistry().size());  // no pack: every face is the missing texture
  {  // Mesher check (P2b-2, review B-L8/D-M9): mesh column (2,2) at ITS OWN
     // topmost sub-chunk that yields quads. The old check meshed (2,2) at
     // the GLOBAL max surfaceSy (a resonator-spire column's height), where
     // (2,2) has no sub-chunk at all — 0 verts, a test bug, not a mesher
     // bug (the mesher's own unit tests pass). Walk (2,2)'s sub-chunks
     // from the top; the first that meshes non-empty is by construction a
     // sub-chunk containing a non-air block with an exposed face.
    const StableId airSid = blockRegistry().stableIdOf(kAir);
    int meshSy = -1;
    size_t meshVerts = 0;
    int triedCensus = 0;
    for (int sy = kSubChunks - 1; sy >= 0 && meshSy < 0; --sy) {
      const ChunkColumn* col = wafer->chunks.column(2, 2);
      if (!col || !col->subs[size_t(sy)]) continue;
      int census = 0;
      for (int i = 0; i < kSubChunkVolume; ++i)
        if (col->subs[size_t(sy)]->blocks.at(i) != airSid) ++census;
      if (census == 0) continue;
      triedCensus = census;
      const ChunkMesh m = meshSubChunk(*snapshotForMeshing(wafer->chunks, blockRegistry(), 2, sy, 2, 1, 1, wafer->belowWorldFill),
                                       MeshContext{blockRegistry(), checker, &blockAppearance()});
      if (!m.verts.empty()) { meshSy = sy; meshVerts = m.verts.size(); }
    }
    if (meshSy < 0)
      std::printf("  mesher diag: column (2,2) produced no quads at any sub-chunk (last tried census=%d non-air cells)\n", triedCensus);
    else
      std::printf("  mesher: column (2,2) own surface sub-chunk sy=%d -> %zu verts\n", meshSy, meshVerts);
    check(meshSy >= 0 && meshVerts % 4 == 0, "mesher produces quads for a surface sub-chunk");
  }
  {
    HudLayer hud;
    hud.rects.push_back({0, 0, 10, 10, 1, 1, 1, 1});
    hud.texts.push_back({4, 4, "ab"});
    std::vector<HudVertex> v;
    buildHudVertices(hud, v);
    const HudFontAtlas atlas = buildHudFontAtlas();
    check(v.size() == 30 && v[0].u < 0 && v[6].u >= 0 && atlas.pixels.size() == size_t(atlas.width) * size_t(atlas.height),
          "overlay geometry (shared by the GL and Vulkan backends): rect + shadowed text quads, font atlas");
  }
  {
    const VulkanProbe vk = probeVulkan();  // headless: no window, instance + device list only
    if (!vk.loaderFound) std::printf("  [SKIP] vulkan: %s\n", vk.summary.c_str());
    // Loader present but no usable device on this machine (GPU-less box:
    // VK_ERROR_INCOMPATIBLE_DRIVER / 0 devices) is environmental — SKIP,
    // not a permanent FAIL that trains readers to ignore the suite
    // (P2b-2, review G-L6). Genuine init failures still FAIL via vk.ok.
    else if (vk.noDevices) std::printf("  [SKIP] vulkan: no usable device on this machine (%s)\n", vk.summary.c_str());
    else check(vk.ok, ("vulkan: headless instance + physical devices (" + vk.summary + ")").c_str());
  }
  wafer->unload();
  check(wafer->chunks.columns().empty(), "wafer unload frees all chunks");

  ow->load(pool, 8);
  int biome[2] = {0, 0};
  for (auto& [k, col] : ow->chunks.columns())
    for (auto& s : col->subs)
      if (s) for (int i = 0; i < kSubChunkVolume; ++i) if (s->blocks.at(i) == blockRegistry().stableIdOf(kGrass)) ++biome[s->state[i] & 1];
  check(ow->chunks.columns().size() == 64 && reg.find("overworld") == ow && biome[0] > 0 && biome[1] > 0, "organic world loads with both biomes (plains + hills); 'overworld' alias works");
  {  // worldgen: built-ins embedded in the exe, seeding, caves + veins underground
    int caves = 0, dirtVeins = 0;
    for (auto& [k, col] : ow->chunks.columns())
      for (int z = 0; z < kChunkSize; ++z)
        for (int x = 0; x < kChunkSize; ++x) {
          const int wx = col->cx * kChunkSize + x, wz = col->cz * kChunkSize + z, top = ow->chunks.surfaceY(wx, wz);
          for (int y = 1; y < top - 12; ++y) {
            const BlockId b = ow->chunks.get(wx, y, wz);
            caves += b == kAir;
            dirtVeins += b == kDirt;
          }
        }
    check(caves > 0 && dirtVeins > 0, "organic worldgen: worm caves and dirt veins underground, surface intact");
    WorldgenSettings seeded;
    seeded.seed = 12345;
    DimensionRegistry other = createDimensions(seeded);
    const auto a = ow->generator->generate(3, 3), b = other.find("organic")->generator->generate(3, 3);
    const auto w0 = wafer->generator->generate(1, 1), w1 = other.find("wafer")->generator->generate(1, 1);
    int organicDiff = 0, waferDiff = 0;
    for (int sy = 0; sy < kSubChunks; ++sy) {
      organicDiff += bool(a->subs[size_t(sy)]) != bool(b->subs[size_t(sy)]) ||
                     (a->subs[size_t(sy)] && a->subs[size_t(sy)]->blocks != b->subs[size_t(sy)]->blocks);
      waferDiff += bool(w0->subs[size_t(sy)]) != bool(w1->subs[size_t(sy)]) ||
                   (w0->subs[size_t(sy)] && w0->subs[size_t(sy)]->blocks != w1->subs[size_t(sy)]->blocks);
    }
    check(builtinWorldgenSource("organic") && organicDiff > 0 && waferDiff == 0,
          "worldgen: built-in generators embedded; world seed changes organic terrain, wafer is seed-independent");
  }
  ow->unload();

  {  // scripts folder choice (dev builds: source tree; shipping: next to the exe; --scripts overrides)
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = uniqueTempDir("lithokraft-selftest-scripts");  // G-M7: per-run unique
    const fs::path src = root / "src" / "scripts", exe = root / "exe" / "scripts";
    fs::create_directories(src, ec);
    fs::create_directories(exe, ec);
    const auto pick = [&](std::string_view mode, const fs::path& source) { return fs::path(resolveScriptsDir(mode, source, {root / "missing", exe}).dir); };
    check(pick("", src) == src && pick("", fs::path()) == exe && pick("", root / "gone") == exe,
          "scripts dir: dev build edits the source tree; shipping (or no source tree) uses the exe copy");
    check(pick("exe", src) == exe && pick("source", src) == src && pick("source", fs::path()).empty() &&
              pick(exe.string(), src) == exe && pick((root / "nope").string(), src).empty(),
          "scripts dir: --scripts exe|source|<dir> overrides; missing folders give none");
    fs::remove_all(root, ec);
  }

  {  // shipped packs through the resource system: engine base (shaders) < game pack < pack_64 (resources.json)
    ResourceManager rm(packDirs);
    rm.reload();
    const bool ok = packDirs.size() >= 3;
    if (!ok) std::printf("  (engine or game assets/ not found)\n");
    const WorldgenSettings gen = rm.worldgen(1);
    check(ok && gen.generators.count("wafer") && gen.generators.count("organic"),
          "game pack: worldgen/wafer.json and worldgen/organic.json resolve through the pack stack");
    size_t icons = 0;
    for (size_t b = 1; b < blockRegistry().size(); ++b)
      if (const Image* i = rm.icon(BlockId(b)); i && i->width() == 16) ++icons;
      else std::printf("  (no icon for %s: textures/ui/icons/%s.png)\n", blockRegistry().idOf(BlockId(b)).c_str(), ResourceManager::iconName(BlockId(b)).c_str());
    check(ok && icons == blockRegistry().size() - 1, "hotbar: every block has a textures/ui/icons icon in the pack stack (16 x 16 for the HUD)");
    {  // Item icons (items workstream, 2026-10-12): every non-block item in the
       // registry resolves a 16x16 icon through the same pack path; the
       // block-backed ChipOR resolves through the block icon above.
      size_t itemIcons = 0, itemDefs = 0;
      for (const ItemDef& d : itemRegistry()) {
        if (d.itemId < kFirstItemId) continue;  // block-backed (ChipOR)
        ++itemDefs;
        if (const Image* i = rm.itemIcon(d.itemId); i && i->width() == 16) ++itemIcons;
        else std::printf("  (no item icon for %.*s: textures/ui/icons/%.*s.png)\n", int(d.stringId.size()), d.stringId.data(), int(d.iconName.size()), d.iconName.data());
      }
      check(ok && itemIcons == itemDefs, "items: every non-block item has a textures/ui/icons icon in the pack stack (16 x 16 for the HUD)");
    }
    {  // Fresh spawn inventory (items workstream): the Fragment is granted on
       // spawn and a fresh Inventory can hold every lore tool item.
      Inventory inv;
      bool fresh = grantStartingItems(inv) && inv.countOf(kItemFragment) == 1 &&
                   inv.hotbar()[0].itemId == kItemFragment;
      for (const ItemDef& d : itemRegistry())
        if (!d.startingItem) fresh = fresh && inv.addItem(d.itemId, 1) == 0 && inv.countOf(d.itemId) >= 1;
      check(fresh, "items: fresh spawn inventory holds the Fragment + every lore tool item");
    }
    {
      HudLayer hud;
      HudUiRenderer r(hud, {}, [&](std::string_view) { return rm.icon(kRedTorch); });
      r.drawImage({0, 0, 32, 32}, "block:4", {1, 1, 1, 1});
      check(hud.draws.size() > 4, "hotbar: a block icon paints as pixel rects, not a single swatch");
    }
    const TextureRegistry& tex = rm.textures();
    const auto mats = rm.materials();
    auto face = [&](BlockId b, FaceSlot f, bool active) { return tex.texture(TextureId{mats->face(b, f, active).layer}).name; };
    check(ok && tex.textures().size() > 80 && tex.missingNames().empty() && rm.models().size() >= 24,
          "pack_64 resources.json: every texture loads into the registry, 24+ block models");
    check(ok && face(kRepeater, FaceSlot::Front, false) == "repeater_front" && face(kRepeater, FaceSlot::Top, true) == "repeater_top_on" &&
              face(kThresholdor, FaceSlot::Left, false) == "thresholdor_low" && face(kThresholdor, FaceSlot::Right, false) == "thresholdor_high" &&
              face(kSwitch, FaceSlot::Bottom, false) == "switch_side" && face(kSiliconSlab, FaceSlot::Bottom, false) == "silicon_slab_top" &&
              face(kMemor, FaceSlot::Top, false) == "memor_top_empty" && face(kMemor, FaceSlot::Top, true) == "memor_top",
          "block models: front/back/left/right files, active top, side and slab fallbacks (as before the refactor)");
    const auto anim = tex.findAnimation("pulsor_top_firing");
    check(ok && anim && tex.animation(*anim).frameCount == 6 && tex.animation(*anim).timing.frameMs == 50 &&
              mats->face(kPulsor, FaceSlot::Top, true).animated() && !mats->face(kPulsor, FaceSlot::Top, false).animated(),
          "pulsor firing strip: 6 frames at 50 ms from its .json sidecar, animated only while active");
    const auto toneAnim = tex.findAnimation("tone_block_front_playing");
    check(ok && toneAnim && tex.animation(*toneAnim).frameCount == 4 && tex.animation(*toneAnim).timing.frameMs == 100 &&
              tex.animation(*toneAnim).timing.loop && mats->face(kToneBlock, FaceSlot::Front, true).animated() &&
              face(kToneBlock, FaceSlot::Front, false) == "tone_block_front" && face(kToneBlock, FaceSlot::Top, true) == "tone_block_top_playing" &&
              face(kToneBlock, FaceSlot::Left, false) == "tone_block_side",
          "tone block (art v1.8): idle faces, playing top, 4-frame 100 ms looping front strip");
    const MaterialBindings& b = mats->bindings();
    check(ok && tex.palettes().size() == 5 && face(kRedTrace, FaceSlot::Top, false) == "missing" &&
              tex.texture(TextureId{mats->wirePiece(kBlueTrace, WirePiece::Corner).layer}).name == "blue_trace_corner" &&
              tex.palette(PaletteId{uint16_t(b.stripPalette[0])}).name == "red_trace" &&
              tex.palette(PaletteId{uint16_t(b.tierPalette[2])}).name == "vga64" &&
              tex.texture(TextureId{uint16_t(b.stripRing[1])}).name == "blue_trace_ring_mask" &&
              tex.texture(TextureId{uint16_t(b.pipMask)}).name == "repeater_delay_mask",
          "materials: wire pieces, palettes and shader masks resolved from the models (no names in C++)");
    const ShaderLibrary& sh = rm.shaders();
    check(ok && sh.size() == 3 && sh.find("chunk") && sh.find("chunk")->origin == "base" &&
              sh.find("chunk")->fragment.find("uWirePal") != std::string::npos && sh.find("hud") && sh.find("line"),
          "shaders: chunk / hud / line load from assets/shaders through the pack stack");
  }
  {  // HUD readout comes from each block's behaviour class, through the registry
    const auto& cat = BehaviorCatalog::get();
    check(cat.describe(kRepeater, makeState(64, 1, 3, true)) == "Repeater | delay 4 | out 64 | facing E" &&
              cat.describe(kRedTrace, 40) == "Red Trace | strength 40" &&
              cat.describe(kColorLamp16, makeState(17, 0)) == "Color Lamp 16 | tier 16 (wrap) | strength 17 -> colour 0" &&
              cat.describe(kToneBlock, makeState(34, 2)).find("A4 (440.0 Hz)") != std::string::npos &&
              cat.describe(kWaferSilicon, 0) == "Wafer Silicon" && cat.component(kRepeater) && !cat.component(kRedTrace),
          "look-at readout: describe() per block behaviour (repeater, wire, color lamp, tone, plain)");
  }
  {  // AngelScript: hello.as loads, onLoad() runs and logs through our API
    ScriptEngine se;
    bool ok = se.init();
    se.setModPermissions(kPermWorldRead);  // currentDimension() needs world-read
    ok = ok && se.bindGlobalFunction("string currentDimension()", asFUNCTION(stubDimension), asCALL_CDECL) >= 0;
    ok = ok && se.loadDirectory(scriptsDir) && se.callHook("void onLoad()");
    check(ok && se.logCount() == 1 && se.lastLog().find("Hello") != std::string::npos,
          "AngelScript: scripts/hello.as loads and onLoad() logs");
  }
  {  // sandbox: no unregistered functions; runaway loops are aborted
    ScriptEngine se;
    se.init();
    std::printf("  (the next script error is expected)\n");
    check(!se.loadSource("sandbox", "void onLoad() { system(\"calc\"); }"), "AngelScript sandbox: unregistered system() call does not compile");
    ScriptEngine se2;
    se2.init();
    check(se2.loadSource("loop", "void onLoad() { while (true) {} }") && !se2.callHook("void onLoad()"),
          "AngelScript sandbox: infinite loop aborted by time budget");
  }
  {  // live log file: tee + per-line flush + rotation to .prev.log
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = uniqueTempDir("lithokraft-selftest-logs");  // G-M7: per-run unique
    fs::remove_all(dir, ec);
    const fs::path f = dir / "lithokraft.log";
    auto slurp = [](const fs::path& p) { std::ifstream in(p); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
    bool ok = logOpenFile(f.string());
    logInfo("selftest run one");
    const std::string live = slurp(f);  // readable before close = flushed per line
    ok = ok && logOpenFile(f.string());
    logInfo("selftest run two");
    logCloseFile();
    const std::string cur = slurp(f), prev = slurp(dir / "lithokraft.prev.log");
    check(ok && live.find("selftest run one") != std::string::npos && live[0] == '[',
          "log file: timestamped lines, flushed per line (tail-able)");
    check(cur.find("run two") != std::string::npos && cur.find("run one") == std::string::npos &&
          prev.find("run one") != std::string::npos, "log file: previous run rotated to lithokraft.prev.log");
    fs::remove_all(dir, ec);
  }
  {  // SGNL v1 -> v2 migration: channel/beacon inserted before the key count
    storage::ByteWriter v1;
    v1.u64(0x123456789ABCDEF0ULL);
    for (int i = 0; i < 8; ++i) v1.u8(uint8_t(10 + i));  // out..timer
    v1.u16(2);
    v1.u8(7); v1.u8(8); v1.u8(9); v1.u8(10);  // two key pairs
    auto v2 = WorldSave::migrateSignalV1ToV2(v1.out);
    bool ok = v2.has_value();
    if (ok) {
      storage::ByteReader r(v2->data(), v2->size());
      ok = r.u64() == 0x123456789ABCDEF0ULL;
      for (int i = 0; i < 8 && ok; ++i) ok = r.u8() == uint8_t(10 + i);
      ok = ok && r.u8() == 1 && r.u8() == 0;  // channel=1, beacon=0
      ok = ok && r.u16() == 2;
      ok = ok && r.u8() == 7 && r.u8() == 8 && r.u8() == 9 && r.u8() == 10;
      ok = ok && r.done();
    }
    check(ok, "save: v1 -> v2 migration inserts channel/beacon before the key count");
    storage::Bytes bad{1, 2, 3};  // truncated: must fail, not corrupt
    check(!WorldSave::migrateSignalV1ToV2(bad).has_value(), "save: malformed v1 state fails migration cleanly");
  }
  if (timingHandshake) std::printf("  (selftest invoked with --timing-handshake: the handshake section below A/B-compares both modes in-process)\n");
  fails += runHandshakeSelfTest();
  fails += runControlSelfTest();
  fails += runDemoSelfTest();
  std::printf("%s\n", fails ? "ALL SELFTESTS: FAILED" : "ALL SELFTESTS: OK");
  return fails ? 1 : 0;
}

}  // namespace lithokraft
