// text_indexer.cpp: inverted index over text corpora. Simple and obviously
// correct: per-document term frequencies plus term -> (doc -> tf) postings.
// Scoring is sum-of-term-frequencies with a DocId tie-break — deterministic
// and easy to reason about for the future defrag service.
#include "litholattice/index/text_indexer.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace litholattice::index {
namespace {

// UTF-8 decode that never splits a multibyte sequence. Malformed bytes are
// treated as single code points (tolerate, don't crash) and counted as
// term characters so nothing is silently dropped.
size_t decodeCodePoint(const char* p, size_t avail, uint32_t* out) {
  const uint8_t b0 = static_cast<uint8_t>(p[0]);
  if (b0 < 0x80) {
    *out = b0;
    return 1;
  }
  size_t want = 0;
  uint32_t cp = 0;
  if ((b0 & 0xE0) == 0xC0) {
    want = 2;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    want = 3;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    want = 4;
    cp = b0 & 0x07;
  } else {
    // Stray continuation byte or 0xFE/0xFF: take it as-is.
    *out = b0;
    return 1;
  }
  if (avail < want) {
    // Truncated tail: consume the remaining bytes as a single unit.
    *out = b0;
    return avail;
  }
  for (size_t i = 1; i < want; ++i) {
    const uint8_t b = static_cast<uint8_t>(p[i]);
    if ((b & 0xC0) != 0x80) {
      // Bad continuation: consume the lead byte only, re-scan the rest.
      *out = b0;
      return 1;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  *out = cp;
  return want;
}

// A code point is a term character when it is ASCII alphanumeric, or any
// non-ASCII code point (terms are runs of alphanumeric code points).
bool isTermChar(uint32_t cp) {
  if (cp < 0x80) return std::isalnum(static_cast<int>(cp)) != 0;
  return true;
}

// Tokenize text into normalized terms (ASCII folded to lowercase; UTF-8
// bytes of non-ASCII code points pass through unchanged).
void tokenize(std::string_view text, std::vector<std::string>* terms) {
  const char* p = text.data();
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    uint32_t cp = 0;
    const size_t len = decodeCodePoint(p + i, n - i, &cp);
    if (isTermChar(cp)) {
      std::string term;
      while (i < n) {
        uint32_t cp2 = 0;
        const size_t len2 = decodeCodePoint(p + i, n - i, &cp2);
        if (!isTermChar(cp2)) break;
        if (cp2 < 0x80) {
          term.push_back(static_cast<char>(std::tolower(static_cast<int>(cp2))));
        } else {
          term.append(p + i, len2);
        }
        i += len2;
      }
      terms->push_back(std::move(term));
    } else {
      i += len;
    }
  }
}

}  // namespace

void TextIndexer::addDocument(DocId id, std::string_view text) {
  removeDocument(id);  // replace: drop the old postings first
  std::vector<std::string> terms;
  tokenize(text, &terms);
  std::unordered_map<std::string, uint64_t> tf;
  tf.reserve(terms.size());
  for (const auto& t : terms) ++tf[t];
  docs_.emplace(id, tf);
  for (const auto& [term, count] : tf) index_[term][id] = count;
  ++docCount_;
}

void TextIndexer::removeDocument(DocId id) {
  auto docIt = docs_.find(id);
  if (docIt == docs_.end()) return;  // missing id = no-op
  for (const auto& kv : docIt->second) {
    const std::string& term = kv.first;
    auto idxIt = index_.find(term);
    if (idxIt == index_.end()) continue;
    idxIt->second.erase(id);
    if (idxIt->second.empty()) index_.erase(idxIt);
  }
  docs_.erase(docIt);
  --docCount_;
}

std::vector<TextIndexer::Hit> TextIndexer::query(std::string_view q, size_t maxResults) const {
  std::vector<std::string> terms;
  tokenize(q, &terms);
  if (terms.empty()) return {};  // empty query -> empty result

  std::unordered_map<DocId, double> scores;
  for (const auto& term : terms) {
    auto idxIt = index_.find(term);
    if (idxIt == index_.end()) continue;
    for (const auto& [doc, count] : idxIt->second) {
      scores[doc] += static_cast<double>(count);
    }
  }
  std::vector<Hit> hits;
  hits.reserve(scores.size());
  for (const auto& [doc, score] : scores) hits.push_back(Hit{doc, score});
  // Deterministic order: score descending, DocId ascending on ties.
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.doc < b.doc;
  });
  if (hits.size() > maxResults) hits.resize(maxResults);
  return hits;
}

size_t TextIndexer::documentCount() const { return docCount_; }

size_t TextIndexer::termCount() const { return index_.size(); }

void TextIndexer::clear() {
  docs_.clear();
  index_.clear();
  docCount_ = 0;
}

}  // namespace litholattice::index
