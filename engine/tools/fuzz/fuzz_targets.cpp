// Fuzz harnesses for the loaders that read untrusted files: PNG, JSON (pack manifests and every
// other JSON file), generator graphs, save regions and chunk records.
// Each target has the libFuzzer signature (build with clang -fsanitize=fuzzer and
// -DLITHOLATTICE_LIBFUZZER=<target index>). Without libFuzzer, the built-in driver below mutates
// seed files for N iterations (AFL-style byte flips, inserts, splices). Run it under ASan / UBSan.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>
#include "litholattice/assets/image.h"
#include "litholattice/core/json.h"
#include "litholattice/procgen/generator_loader.h"
#include "litholattice/storage/storage.h"

using namespace litholattice;

namespace {

void fuzzPng(const uint8_t* d, size_t n) {
  std::string err;
  (void)Image::decode(std::span<const uint8_t>(d, n), &err);
}
void fuzzJson(const uint8_t* d, size_t n) {
  Json j; std::string err;
  if (Json::parse(std::string(reinterpret_cast<const char*>(d), n), j, &err)) (void)j.dump();
}
void fuzzGenerator(const uint8_t* d, size_t n) {
  std::string err;
  auto resolve = [](std::string_view name) -> std::optional<procgen::MaterialId> {
    if (name == "air") return procgen::MaterialId(0);
    if (name.size() < 32) return procgen::MaterialId(1 + name.size() % 7);
    return std::nullopt;
  };
  auto g = procgen::loadGenerator(std::string(reinterpret_cast<const char*>(d), n), 1234, resolve, err);
  (void)g;
}
void fuzzRecord(const uint8_t* d, size_t n) {
  static const storage::PayloadRegistry reg;
  std::string err;
  (void)reg.decode(storage::Bytes(d, d + n), &err);
}
void fuzzRegion(const uint8_t* d, size_t n) {
  static const auto path = std::filesystem::temp_directory_path() / "litholattice_fuzz_region.llr";
  { std::ofstream f(path, std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(d), std::streamsize(n)); }
  storage::RegionFile r(path);
  std::string err;
  if (!r.open(&err)) return;
  static const storage::PayloadRegistry reg;
  for (int z = 0; z < storage::RegionFile::kSpan; ++z)
    for (int x = 0; x < storage::RegionFile::kSpan; ++x)
      if (auto rec = r.read(x, z, &err)) (void)reg.decode(*rec, &err);
}

struct Target { const char* name; void (*fn)(const uint8_t*, size_t); };
constexpr Target kTargets[] = {{"png", fuzzPng}, {"json", fuzzJson}, {"generator", fuzzGenerator}, {"record", fuzzRecord}, {"region", fuzzRegion}};

}  // namespace

#ifdef LITHOLATTICE_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { kTargets[LITHOLATTICE_LIBFUZZER].fn(d, n); return 0; }
#else
int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: litholattice_fuzz <png|json|generator|record|region> <iterations> [seed files...]\n");
    return 2;
  }
  const Target* t = nullptr;
  for (const Target& x : kTargets) if (!std::strcmp(x.name, argv[1])) t = &x;
  if (!t) { std::fprintf(stderr, "unknown target %s\n", argv[1]); return 2; }
  const long iterations = std::atol(argv[2]);
  std::vector<std::vector<uint8_t>> seeds;
  for (int i = 3; i < argc; ++i) {
    std::ifstream f(argv[i], std::ios::binary);
    seeds.emplace_back((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  if (seeds.empty() && (t->fn == fuzzRecord || t->fn == fuzzRegion)) {  // built-in seed: a real record / region
    ChunkColumn c; c.cx = 3; c.cz = -2;
    c.subs[1] = std::make_unique<SubChunk>();
    for (int i = 0; i < kSubChunkVolume; i += 3) { c.subs[1]->blocks[size_t(i)] = BlockId(1 + i % 4); c.subs[1]->state[size_t(i)] = CellState(i & 15); }
    c.subs[1]->nonAir = 1366;
    static const storage::PayloadRegistry reg;
    storage::Bytes rec = reg.encode(c);
    if (t->fn == fuzzRecord) seeds.push_back(rec);
    else {
      const auto p = std::filesystem::temp_directory_path() / "litholattice_fuzz_seed.llr";
      std::filesystem::remove(p);
      storage::RegionFile r(p); std::string err; r.open(&err); r.put(3, 30, rec); r.put(0, 0, rec); r.flush(&err);
      std::ifstream f(p, std::ios::binary);
      seeds.emplace_back((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
  }
  if (seeds.empty()) seeds.push_back({});
  std::mt19937_64 rng(0x11A77);
  const std::string crashFile = std::string("fuzz-") + t->name + "-last-input.bin";
  for (long it = 0; it < iterations; ++it) {
    std::vector<uint8_t> in = seeds[rng() % seeds.size()];
    const int edits = 1 + int(rng() % 8);
    for (int e = 0; e < edits; ++e) {
      const unsigned op = unsigned(rng() % 6);
      const size_t pos = in.empty() ? 0 : size_t(rng() % in.size());
      if (op == 0 && !in.empty()) in[pos] ^= uint8_t(1u << (rng() % 8));                 // bit flip
      else if (op == 1 && !in.empty()) in[pos] = uint8_t(rng());                          // random byte
      else if (op == 2) in.insert(in.begin() + std::ptrdiff_t(pos), uint8_t(rng()));      // insert
      else if (op == 3 && !in.empty()) in.erase(in.begin() + std::ptrdiff_t(pos));        // delete
      else if (op == 4 && !in.empty()) in.resize(size_t(rng() % (in.size() + 1)));        // truncate
      else if (op == 5 && in.size() > 4) {                                                // interesting values
        static const uint32_t kVals[] = {0, 1, 0x7f, 0xff, 0xffff, 0x7fffffff, 0xffffffff, 4096, 65536};
        const uint32_t v = kVals[rng() % std::size(kVals)];
        std::memcpy(in.data() + std::min(pos, in.size() - 4), &v, 4);
      }
    }
    // keep the input that is about to run, so a crash can be replayed
    { std::ofstream f(crashFile, std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(in.data()), std::streamsize(in.size())); }
    t->fn(in.data(), in.size());
  }
  std::filesystem::remove(crashFile);
  std::printf("fuzz %s: %ld iterations, no crash\n", t->name, iterations);
  return 0;
}
#endif
