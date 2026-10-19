// texture_gen: generate block textures from procedural recipes into a pack.
//
//   texture_gen --recipes <dir> --pack <packDir> --size <n> [--subdir <rel>] [--subdir <rel>]
//
// For each *.ltxr file in <dir> (sorted by name for deterministic output):
// parse it with litholattice::parseRecipe, generate at <n>x<n> with a FIXED
// per-texture seed, and write <packDir>/<subdir>/<name>.png, where
// <name> is the recipe file stem (== the texture / PNG name in the packs).
// <subdir> defaults to textures/blocks; item icons are generated with
// --subdir textures/ui/icons --size 16 from assets/texture_recipes/items/.
//
// Per-texture seed: FNV-1a 64-bit hash of the texture name
// (offset basis 14695981039346656037, prime 1099511628211, over the name's
// bytes, no terminator). Deterministic across runs and machines, so the same
// recipes always produce byte-identical PNGs in every pack size.
//
// Recipes are data files (game/LithoKraft/assets/texture_recipes/*.ltxr) --
// the future player/mod authoring format; see texture_recipe.h for the
// line-based node syntax. Not a ctest entry; run manually per pack size.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "litholattice/texture/texture_generator.h"
#include "litholattice/texture/texture_recipe.h"

namespace fs = std::filesystem;

namespace {

uint64_t seedForName(const std::string& name) {
  uint64_t h = 14695981039346656037ULL;  // FNV-1a 64 offset basis
  for (const unsigned char c : name) {
    h ^= c;
    h *= 1099511628211ULL;  // FNV-1a 64 prime
  }
  return h;
}

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  fs::path recipesDir, packDir;
  fs::path subdir = "textures/blocks";
  int size = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "texture_gen: %s needs a value\n", flag);
        return {};
      }
      return argv[++i];
    };
    if (arg == "--recipes") recipesDir = next("--recipes");
    else if (arg == "--pack") packDir = next("--pack");
    else if (arg == "--subdir") subdir = next("--subdir");
    else if (arg == "--size") {
      const std::string v = next("--size");
      try {
        size = std::stoi(v);
      } catch (...) {
        size = 0;
      }
    } else {
      std::fprintf(stderr, "texture_gen: unknown argument '%s'\n", arg.c_str());
      std::fprintf(stderr, "usage: texture_gen --recipes <dir> --pack <packDir> --size <n> [--subdir <rel>]\n");
      return 2;
    }
  }
  if (recipesDir.empty() || packDir.empty() || size <= 0) {
    std::fprintf(stderr, "usage: texture_gen --recipes <dir> --pack <packDir> --size <n> [--subdir <rel>]\n");
    return 2;
  }
  std::error_code ec;
  if (!fs::is_directory(recipesDir, ec)) {
    std::fprintf(stderr, "texture_gen: recipes dir not found: %s\n", recipesDir.string().c_str());
    return 1;
  }
  const fs::path outDir = packDir / subdir;
  fs::create_directories(outDir, ec);
  if (ec) {
    std::fprintf(stderr, "texture_gen: cannot create %s: %s\n", outDir.string().c_str(), ec.message().c_str());
    return 1;
  }

  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(recipesDir, ec))
    if (entry.is_regular_file() && entry.path().extension() == ".ltxr") files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    std::fprintf(stderr, "texture_gen: no *.ltxr recipes in %s\n", recipesDir.string().c_str());
    return 1;
  }

  int failures = 0;
  double totalMs = 0.0;
  for (const fs::path& file : files) {
    const std::string name = file.stem().string();
    const std::string text = readFile(file);
    litholattice::TextureRecipe recipe;
    std::string err;
    if (text.empty() || !litholattice::parseRecipe(text, &recipe, &err)) {
      std::fprintf(stderr, "texture_gen: %s: %s\n", file.string().c_str(),
                   text.empty() ? "cannot read file" : err.c_str());
      ++failures;
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const litholattice::Image img = litholattice::generateTexture(recipe, seedForName(name), size);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    totalMs += ms;
    if (img.empty()) {
      std::fprintf(stderr, "texture_gen: %s: generation produced an empty image\n", name.c_str());
      ++failures;
      continue;
    }
    const fs::path out = outDir / (name + ".png");
    if (!img.savePng(out, &err)) {
      std::fprintf(stderr, "texture_gen: %s: %s\n", out.string().c_str(), err.c_str());
      ++failures;
      continue;
    }
    std::printf("%s: %.2f ms (seed 0x%016llx) -> %s\n", name.c_str(), ms,
                (unsigned long long)seedForName(name), out.string().c_str());
  }
  std::printf("texture_gen: %zu texture(s), %d failure(s), total %.2f ms\n", files.size(), failures, totalMs);
  return failures ? 1 : 0;
}
