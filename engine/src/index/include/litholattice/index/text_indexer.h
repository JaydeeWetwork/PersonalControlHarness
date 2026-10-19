#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace litholattice::index {
// Inverted index over text corpora (Fragment journal pages, unlock log,
// recipes, chip designs — any DocId-keyed corpus). Incremental: addDocument
// reindexes only that document. UTF-8 safe: multibyte sequences are never
// split; ASCII case-folded, terms are runs of alphanumeric code points.
class TextIndexer {
 public:
  using DocId = uint64_t;
  struct Hit { DocId doc = 0; double score = 0.0; };  // score desc, doc asc on ties
  void addDocument(DocId id, std::string_view text);  // insert or replace
  void removeDocument(DocId id);                        // missing id = no-op
  std::vector<Hit> query(std::string_view q, size_t maxResults = 64) const;  // deterministic
  size_t documentCount() const;
  size_t termCount() const;  // diagnostics
  void clear();
  // FRAGMENT SEARCH SEAM (Fragment UI doesn't exist yet — design only):
  // the future search button+box calls query(boxText, 32); the UI maps the
  // returned DocIds to pages. DocId assignment is the UI's business (e.g. a
  // page index); the indexer never interprets ids.
 private:
  // term -> (doc -> term frequency); and per-document term frequencies so
  // addDocument/removeDocument only touch the one document's postings.
  // NOT thread-safe; the owner must serialize access.
  std::unordered_map<std::string, std::unordered_map<DocId, uint64_t>> index_;
  std::unordered_map<DocId, std::unordered_map<std::string, uint64_t>> docs_;
  size_t docCount_ = 0;
};
}  // namespace litholattice::index
