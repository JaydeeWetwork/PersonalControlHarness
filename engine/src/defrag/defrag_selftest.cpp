// defrag_selftest.cpp: headless checks for the defrag module.
#include "litholattice/defrag/defrag_selftest.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "litholattice/defrag/defrag.h"
#include "litholattice/index/text_indexer.h"

namespace litholattice::defrag {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] defrag: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

std::vector<uint8_t> bytesOf(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

// Builds a fragmented store: N records, every other one erased, a quarter
// replaced (tombstoning the originals).
RecordStore makeFragmented(size_t n, size_t recSize) {
  RecordStore s;
  for (size_t i = 0; i < n; ++i) {
    std::string payload = "record-" + std::to_string(i) + "-";
    payload += std::string(recSize > payload.size() ? recSize - payload.size() : 0, 'x');
    s.put(i + 1, bytesOf(payload));
  }
  for (size_t i = 0; i < n; i += 2) s.erase(i + 1);            // half erased
  for (size_t i = 1; i < n; i += 4) {                          // quarter replaced
    std::string payload = "replaced-" + std::to_string(i) + "-";
    payload += std::string(recSize > payload.size() ? recSize - payload.size() : 0, 'y');
    s.put(i + 1, bytesOf(payload));
  }
  return s;
}

// Drives a session to a terminal phase; false when the pump budget is hit.
bool runToCompletion(DefragService& svc, int maxPumps = 100000) {
  for (int i = 0; i < maxPumps && svc.running(); ++i) svc.pump();
  return !svc.running();
}

std::filesystem::path scratchPath(const std::string& name) {
  return std::filesystem::temp_directory_path() / ("litho_defrag_test_" + name);
}

}  // namespace

int runDefragSelfTest() {
  Checker check;

  // 1. Store basics: put/get/erase round-trip.
  {
    RecordStore s;
    check(s.put(1, bytesOf("hello")), "store: put ok");
    auto g = s.get(1);
    check(g && *g == bytesOf("hello"), "store: get round-trips");
    check(!s.get(999).has_value(), "store: missing id -> nullopt");
    check(s.erase(1), "store: erase ok");
    check(!s.get(1).has_value(), "store: erased id -> nullopt");
    check(!s.erase(1), "store: double erase refused");
    std::string big(RecordStore::kMaxRecordSize + 1, 'z');
    check(!s.put(2, bytesOf(big)), "store: oversize put refused fail-closed");
    check(!s.get(2).has_value(), "store: refused put left nothing behind");
  }

  // 2. Fragmentation accounting.
  {
    RecordStore s = makeFragmented(100, 64);
    check(s.deadBytes() > 0, "store: dead bytes accumulate after erase/replace");
    check(s.recordCount() == 50, "store: live record count is 50 (100 - 50 erased; 25 replaced stay live)");
    auto g = s.get(2);  // id 2 = i=1, replaced
    check(g && g->size() == 64 && (*g)[0] == 'r', "store: replaced record readable");
  }

  // 3. Defrag happy path: byte-integrity, dead space reclaimed, fewer pages.
  {
    RecordStore store = makeFragmented(400, 128);
    const size_t pagesBefore = store.pageCount();
    const size_t deadBefore = store.deadBytes();
    // Snapshot expected live content.
    std::unordered_map<uint64_t, std::vector<uint8_t>> expected;
    for (uint64_t id = 1; id <= 400; ++id) {
      auto g = store.get(id);
      if (g) expected[id] = *g;
    }
    check(deadBefore > 0, "defrag: fixture is fragmented");

    DefragService svc;
    const auto path = scratchPath("happy");
    check(svc.registerStore("frag", store, path), "defrag: register ok");
    check(!svc.registerStore("frag", store, path), "defrag: duplicate name refused");
    check(svc.beginDefrag("frag"), "defrag: begin ok");
    check(!svc.beginDefrag("frag"), "defrag: double begin refused fail-closed");
    check(runToCompletion(svc), "defrag: session completes");
    check(svc.phase() == DefragService::Phase::Done, "defrag: phase Done");
    check(svc.progress() == 1.0, "defrag: progress 1.0 at Done");

    bool intact = store.recordCount() == expected.size();
    for (const auto& [id, bytes] : expected) {
      auto g = store.get(id);
      if (!g || *g != bytes) {
        intact = false;
        break;
      }
    }
    check(intact, "defrag: every live record byte-identical after");
    check(store.deadBytes() == 0, "defrag: dead space fully reclaimed");
    check(store.pageCount() <= pagesBefore, "defrag: page count never grows");
    // The committed file loads back identically.
    std::string err;
    auto loaded = RecordStore::load(path, &err);
    check(loaded.has_value(), "defrag: committed file loads (" + err + ")");
    bool fileIntact = loaded && loaded->recordCount() == expected.size();
    if (loaded) {
      for (const auto& [id, bytes] : expected) {
        auto g = loaded->get(id);
        if (!g || *g != bytes) {
          fileIntact = false;
          break;
        }
      }
    }
    check(fileIntact, "defrag: committed file content matches live store");
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  // 4. Reindex through the text indexer: index consistent after defrag.
  {
    RecordStore store;
    index::TextIndexer indexer;
    for (uint64_t i = 1; i <= 50; ++i) {
      std::string text = "page about dragons number " + std::to_string(i);
      store.put(i, bytesOf(text));
      indexer.addDocument(i, text);
    }
    for (uint64_t i = 2; i <= 50; i += 2) {
      store.erase(i);
      indexer.removeDocument(i);
    }
    TextIndexerReindexer rx(&indexer);
    DefragService svc;
    check(svc.registerStore("frag", store, scratchPath("reindex"), &rx),
          "defrag: register with reindexer ok");
    check(svc.beginDefrag("frag"), "defrag: begin with reindexer ok");
    check(runToCompletion(svc), "defrag: reindex session completes");
    check(svc.phase() == DefragService::Phase::Done, "defrag: reindex phase Done");
    auto hits = indexer.query("dragons", 64);
    check(hits.size() == 25, "defrag: index consistent after reindex (25 docs)");
    check(indexer.documentCount() == 25, "defrag: document count matches live records");
  }

  // 5. Crash-safety: the live file is untouched until the atomic commit.
  {
    RecordStore store = makeFragmented(120, 64);
    const auto path = scratchPath("crashsafe");
    std::string err;
    check(store.save(path, &err), "defrag: save ok (" + err + ")");
    auto before = RecordStore::load(path, &err);
    check(before.has_value(), "defrag: pre-defrag file loads");

    DefragService svc;
    check(svc.registerStore("frag", store, path), "defrag: register ok");
    check(svc.beginDefrag("frag"), "defrag: begin ok");
    // Pump only through Scan (never reaching Commit).
    for (int i = 0; i < 1000 && svc.phase() == DefragService::Phase::Scan; ++i)
      svc.pump();
    auto during = RecordStore::load(path, &err);
    bool untouched = during && during->recordCount() == before->recordCount() &&
                     during->deadBytes() == before->deadBytes();
    check(untouched, "defrag: live file untouched before commit");
    check(runToCompletion(svc), "defrag: completes after the check");
    check(svc.phase() == DefragService::Phase::Done, "defrag: Done");
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  // 6. Mutation mid-defrag: bounded restart, data stays intact.
  {
    RecordStore store = makeFragmented(300, 64);
    DefragService svc;
    check(svc.registerStore("frag", store, scratchPath("restart")), "defrag: register ok");
    check(svc.beginDefrag("frag"), "defrag: begin ok");
    svc.pump();  // get the scan underway
    store.put(10001, bytesOf("mid-defrag write"));  // mutate: bumps generation
    check(runToCompletion(svc), "defrag: completes despite mid-defrag mutation");
    check(svc.phase() == DefragService::Phase::Done, "defrag: Done after restart");
    auto g = store.get(10001);
    check(g && *g == bytesOf("mid-defrag write"),
          "defrag: mid-defrag write survives (restart rescanned it)");
  }

  // 7. Pathological churn: mutate on every pump -> bounded failure, no livelock.
  {
    RecordStore store = makeFragmented(200, 64);
    DefragService svc;
    check(svc.registerStore("frag", store, scratchPath("busy")), "defrag: register ok");
    check(svc.beginDefrag("frag"), "defrag: begin ok");
    for (int i = 0; i < 100000 && svc.running(); ++i) {
      svc.pump();
      store.put(20000 + (i % 7), bytesOf("churn"));  // every pump mutates
    }
    check(!svc.running(), "defrag: churn session terminates (no livelock)");
    check(svc.phase() == DefragService::Phase::Failed, "defrag: churn fails closed");
    check(!svc.lastError().empty(), "defrag: churn reports why");
    // The live store is still fully usable after the failed session.
    // (id 2: even, live in the fixture, replaced by "replaced-1-...")
    auto g = store.get(2);
    check(g.has_value() && g->size() == 64 && (*g)[0] == 'r',
          "defrag: store intact after failed session");
  }

  // 8. Per-tick boundedness: one pump never finishes a large defrag.
  {
    RecordStore store = makeFragmented(5000, 128);
    DefragService svc;
    check(svc.registerStore("frag", store, scratchPath("bounded")), "defrag: register ok");
    check(svc.beginDefrag("frag"), "defrag: begin ok");
    double p0 = svc.progress();
    svc.pump();
    double p1 = svc.progress();
    check(svc.running(), "defrag: single pump does not finish a large session");
    check(p1 >= p0, "defrag: progress monotonic");
    check(p1 < 1.0, "defrag: single pump is bounded work");
    svc.cancel();
    check(!svc.running() && svc.phase() == DefragService::Phase::Idle,
          "defrag: cancel idles the session");
  }

  // 9. Unknown store / unregister rules.
  {
    RecordStore store;
    DefragService svc;
    check(!svc.beginDefrag("nope"), "defrag: unknown store refused");
    check(svc.registerStore("a", store, scratchPath("a")), "defrag: register ok");
    check(svc.unregisterStore("a"), "defrag: unregister idle store ok");
    check(!svc.unregisterStore("a"), "defrag: double unregister refused");
    check(svc.registerStore("b", store, scratchPath("b")), "defrag: re-register ok");
    check(svc.beginDefrag("b"), "defrag: begin ok");
    check(!svc.unregisterStore("b"), "defrag: unregister of busy store refused");
    svc.cancel();
    check(svc.unregisterStore("b"), "defrag: unregister after cancel ok");
  }

  // 10. Empty store defrags cleanly.
  {
    RecordStore store;
    DefragService svc;
    check(svc.registerStore("empty", store, scratchPath("empty")), "defrag: register ok");
    check(svc.beginDefrag("empty"), "defrag: begin on empty ok");
    check(runToCompletion(svc), "defrag: empty session completes");
    check(svc.phase() == DefragService::Phase::Done, "defrag: empty -> Done");
    std::error_code ec;
    std::filesystem::remove(scratchPath("empty"), ec);
  }

  // 11. Cancel mid-reindex: the index is completed synchronously (not left
  // cleared-but-partial), and no dirty flag is set for a fresh snapshot.
  {
    RecordStore store;
    index::TextIndexer indexer;
    for (uint64_t i = 1; i <= 600; ++i)
      store.put(i, bytesOf("cancelreindex doc number " + std::to_string(i)));
    TextIndexerReindexer rx(&indexer);
    DefragService svc;
    check(svc.registerStore("frag", store, scratchPath("cancelreindex"), &rx),
          "defrag: register with reindexer ok");
    check(svc.beginDefrag("frag"), "defrag: begin ok");
    while (svc.running() && svc.phase() != DefragService::Phase::Reindex) svc.pump();
    check(svc.phase() == DefragService::Phase::Reindex, "defrag: reached reindex");
    svc.pump();  // partial reindex: reindexBegin() cleared the index, feed incomplete
    svc.cancel();
    check(!svc.running() && svc.phase() == DefragService::Phase::Idle,
          "defrag: cancel mid-reindex idles the session");
    auto hits = indexer.query("cancelreindex", 700);
    check(hits.size() == 600, "defrag: cancel mid-reindex completes the feed (600 docs)");
    check(indexer.documentCount() == 600, "defrag: document count matches after cancel");
    check(!svc.indexDirty(), "defrag: no dirty flag when the snapshot was fresh");
    std::error_code ec;
    std::filesystem::remove(scratchPath("cancelreindex"), ec);
  }

  // 12. Cancel mid-reindex with a stale snapshot: the index is marked dirty
  // (never fed stale bytes), and the next session's reindex heals it.
  {
    RecordStore store;
    index::TextIndexer indexer;
    for (uint64_t i = 1; i <= 600; ++i)
      store.put(i, bytesOf("stalereindex doc number " + std::to_string(i)));
    TextIndexerReindexer rx(&indexer);
    DefragService svc;
    check(svc.registerStore("frag", store, scratchPath("stalereindex"), &rx),
          "defrag: register with reindexer ok");
    check(svc.beginDefrag("frag"), "defrag: begin ok");
    while (svc.running() && svc.phase() != DefragService::Phase::Reindex) svc.pump();
    check(svc.phase() == DefragService::Phase::Reindex, "defrag: reached reindex");
    svc.pump();                       // partial feed; index cleared
    store.put(601, bytesOf("stalereindex late arrival"));  // mutate mid-reindex
    svc.cancel();
    check(svc.indexDirty() && svc.indexDirtyStore() == "frag",
          "defrag: stale cancel marks the index dirty (no stale feed)");
    check(svc.beginDefrag("frag"), "defrag: re-begin ok");
    check(runToCompletion(svc), "defrag: healing session completes");
    check(svc.phase() == DefragService::Phase::Done, "defrag: healing session Done");
    auto hits = indexer.query("stalereindex", 700);
    check(hits.size() == 601, "defrag: next reindex heals the dirty index (601 docs)");
    check(!svc.indexDirty(), "defrag: dirty flag cleared after a full reindex");
    std::error_code ec;
    std::filesystem::remove(scratchPath("stalereindex"), ec);
  }

  return check.fails();
}

}  // namespace litholattice::defrag
