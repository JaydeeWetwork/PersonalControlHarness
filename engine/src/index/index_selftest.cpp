// index_selftest.cpp: headless checks for the text index module.
#include "litholattice/index/index_selftest.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/index/text_indexer.h"

namespace litholattice::index {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] index: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

bool containsDoc(const std::vector<TextIndexer::Hit>& hits, TextIndexer::DocId doc) {
  for (const auto& h : hits)
    if (h.doc == doc) return true;
  return false;
}

bool sameResults(const std::vector<TextIndexer::Hit>& a, const std::vector<TextIndexer::Hit>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].doc != b[i].doc || a[i].score != b[i].score) return false;
  return true;
}

}  // namespace

int runIndexSelfTest() {
  Checker check;

  // ---- basics: index, query, tf scoring, ordering ----
  {
    TextIndexer ix;
    ix.addDocument(1, "redstone torch powers the redstone circuit");
    ix.addDocument(2, "redstone dust");
    ix.addDocument(3, "a quiet page about the void");
    check(ix.documentCount() == 3, "documentCount after three adds");
    check(ix.termCount() > 0, "termCount nonzero after adds");

    auto hits = ix.query("redstone");
    check(hits.size() == 2, "query 'redstone' hits two docs");
    if (hits.size() == 2) {
      check(hits[0].doc == 1 && hits[1].doc == 2, "tf ordering: doc 1 (tf 2) before doc 2 (tf 1)");
      check(hits[0].score > hits[1].score, "scores strictly descending");
    }
    check(ix.query("REDSTONE").size() == 2, "ASCII case-fold: 'REDSTONE' matches 'redstone'");
    auto both = ix.query("ReDsToNe ToRcH");
    check(both.size() == 2, "union scoring: 'redstone torch' also hits the redstone-only doc");
    if (both.size() == 2) {
      check(both[0].doc == 1, "doc with both query terms ranks first");
    }
    check(ix.query("nonsuchwordxyz").empty(), "unknown term -> no hits");
  }

  // ---- tie-break: equal scores -> DocId ascending ----
  {
    TextIndexer ix;
    ix.addDocument(7, "lattice");
    ix.addDocument(3, "lattice");
    ix.addDocument(5, "lattice");
    auto hits = ix.query("lattice");
    check(hits.size() == 3, "three docs with equal tf");
    if (hits.size() == 3) {
      check(hits[0].doc == 3 && hits[1].doc == 5 && hits[2].doc == 7,
            "tie-break orders DocId ascending");
    }
  }

  // ---- replace: addDocument on an existing id reindexes only that doc ----
  {
    TextIndexer ix;
    ix.addDocument(1, "old words about copper");
    ix.addDocument(2, "copper wire and tin");
    check(containsDoc(ix.query("copper"), 1), "doc 1 found under 'copper' before replace");
    ix.addDocument(1, "brand new text about diamonds");
    check(!containsDoc(ix.query("copper"), 1), "old terms gone after replace");
    check(containsDoc(ix.query("copper"), 2), "other doc's postings untouched by replace");
    check(containsDoc(ix.query("diamonds"), 1), "new terms indexed after replace");
    check(ix.documentCount() == 2, "replace does not change documentCount");
  }

  // ---- remove ----
  {
    TextIndexer ix;
    ix.addDocument(1, "alpha beta");
    ix.addDocument(2, "alpha gamma");
    ix.removeDocument(1);
    check(ix.documentCount() == 1, "documentCount after remove");
    check(!containsDoc(ix.query("alpha"), 1), "removed doc no longer returned");
    check(containsDoc(ix.query("alpha"), 2), "surviving doc still returned");
    ix.removeDocument(999);  // missing id = no-op
    check(ix.documentCount() == 1, "removeDocument(missing) is a no-op");
    ix.removeDocument(2);
    check(ix.documentCount() == 0 && ix.termCount() == 0, "all terms pruned when last doc leaves");
  }

  // ---- unicode safety: multibyte terms index and match, never split ----
  {
    TextIndexer ix;
    ix.addDocument(1, "caf\u00e9 na\u00efve r\u00e9sum\u00e9");
    ix.addDocument(2, "plain ascii page");
    check(containsDoc(ix.query("caf\u00e9"), 1), "multibyte term 'caf\u00e9' matches");
    check(containsDoc(ix.query("na\u00efve"), 1), "multibyte term 'na\u00efve' matches");
    check(!containsDoc(ix.query("cafe"), 1), "no accent folding: 'cafe' does not match 'caf\u00e9'");
    check(ix.query("r\u00e9sum\u00e9").size() == 1, "multibyte term matches exactly one doc");

    // Hostile inputs: truncated multibyte tail, stray continuation bytes,
    // lone lead byte, embedded NUL-ish garbage — must not crash or split.
    std::string nasty;
    nasty += "ok ab\xC3";        // truncated 2-byte sequence at end
    nasty += " \x80\x81";        // stray continuation bytes
    nasty += " \xE2\x82";        // truncated 3-byte sequence
    nasty += " \xFF\xFE junk";   // invalid lead bytes
    nasty += " tail";
    ix.addDocument(3, nasty);
    check(containsDoc(ix.query("tail"), 3), "doc with malformed UTF-8 still indexes tail terms");
    check(ix.query(nasty).size() >= 1, "querying malformed bytes back does not crash");
    ix.addDocument(4, "");
    check(ix.documentCount() == 4, "empty document still counts as a document");
    check(ix.query("").empty(), "empty query -> empty result");
  }

  // ---- empty / punctuation-only queries ----
  {
    TextIndexer ix;
    ix.addDocument(1, "some words here");
    check(ix.query("").empty(), "empty query string -> no hits");
    check(ix.query("   \t\n  ").empty(), "whitespace-only query -> no hits");
    check(ix.query("... !!! ---").empty(), "punctuation-only query -> no hits");
    check(ix.query("words", 0).empty(), "maxResults 0 -> empty result");
  }

  // ---- determinism: same query twice -> identical results ----
  {
    TextIndexer ix;
    for (uint64_t i = 0; i < 200; ++i) {
      ix.addDocument(i, "doc number " + std::to_string(i) + " redstone lattice repeater");
    }
    auto a = ix.query("redstone lattice", 1000);
    auto b = ix.query("redstone lattice", 1000);
    check(a.size() == 200, "determinism fixture: 200 docs hit");
    check(sameResults(a, b), "identical query twice -> identical results");
    // Repeated query terms score repeatedly (documented behavior).
    auto single = ix.query("redstone");
    auto doubled = ix.query("redstone redstone");
    check(sameResults(doubled, ix.query("redstone redstone")), "repeated terms are deterministic");
    if (!single.empty() && !doubled.empty()) {
      check(doubled[0].score == 2.0 * single[0].score, "repeated query term sums tf twice");
    }
  }

  // ---- maxResults cap ----
  {
    TextIndexer ix;
    for (uint64_t i = 0; i < 100; ++i) ix.addDocument(i, "shared term here");
    auto capped = ix.query("shared", 5);
    check(capped.size() == 5, "maxResults caps at 5");
    // The top 5 of 100 equal-score docs are the 5 smallest DocIds (tie-break).
    bool ordered = true;
    for (size_t i = 0; i < capped.size(); ++i) ordered &= (capped[i].doc == i);
    check(ordered, "capped results are the top-N by (score desc, doc asc)");
    auto all = ix.query("shared");
    check(all.size() == 64, "default maxResults is 64");
    auto many = ix.query("shared", 200);
    check(many.size() == 100, "maxResults above hit count returns everything");
  }

  // ---- clear ----
  {
    TextIndexer ix;
    ix.addDocument(1, "something");
    ix.clear();
    check(ix.documentCount() == 0 && ix.termCount() == 0, "clear empties the index");
    check(ix.query("something").empty(), "query after clear -> no hits");
  }

  // ---- latency sanity: 10k small docs + 100 queries under a generous bound ----
  {
    TextIndexer ix;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < 10000; ++i) {
      ix.addDocument(i, "journal entry " + std::to_string(i) +
                          " trace wire redstone repeater comparator latch");
    }
    size_t totalHits = 0;
    for (int q = 0; q < 100; ++q) totalHits += ix.query("trace redstone entry", 10000).size();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::printf("  [info] index: 10k docs + 100 queries in %lld ms (%zu hits)\n",
                static_cast<long long>(ms), totalHits);
    check(totalHits == 100 * 10000, "latency fixture: every query hit all 10k docs");
    check(ms < 5000, "10k docs + 100 queries under 5 s (sanity, not a benchmark)");
  }

  return check.fails();
}

}  // namespace litholattice::index
