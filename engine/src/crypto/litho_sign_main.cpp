// litho_sign: JD's mod-signing tool.
//
//   litho_sign keygen --out <prefix>
//       Generates an Ed25519 keypair. Writes <prefix>.priv (JD GUARDS THIS WITH
//       HIS LIFE - it must NEVER ship, never be committed, never leave his
//       offline machine) and <prefix>.pub (this is what gets baked into the
//       engine / handed to Worker B).
//   litho_sign sign --key <prefix>.priv --mod <dir>
//       Hashes the mod per the canonical spec, signs, writes signature.json.
//   litho_sign verify --key <prefix>.pub --mod <dir>
//       Verifies signature.json against the key. Exit 0 = valid, 1 = invalid.
//
// Key files are raw 32-byte Ed25519 keys (seed for .priv, public key for .pub).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "litholattice/crypto/signing.h"

namespace {

int usage() {
  std::printf(
      "usage:\n"
      "  litho_sign keygen --out <prefix>\n"
      "  litho_sign sign   --key <prefix>.priv --mod <dir>\n"
      "  litho_sign verify --key <prefix>.pub  --mod <dir>\n");
  return 2;
}

const char* flag(int argc, char** argv, const char* name) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return nullptr;
}

int doKeygen(const char* prefix) {
  namespace fs = std::filesystem;
  if (!prefix) return usage();
  std::array<uint8_t, 32> seedArr, pubArr;
  if (!litholattice::crypto::generateKeypair(seedArr, pubArr)) {
    std::printf("keygen: RNG failure\n");
    return 1;
  }
  const std::string privPath = std::string(prefix) + ".priv";
  const std::string pubPath = std::string(prefix) + ".pub";
  if (!litholattice::crypto::writePrivateKeyFile(privPath, seedArr.data()) ||
      !litholattice::crypto::writeKeyFile(pubPath, pubArr.data())) {
    std::printf("keygen: write failed\n");
    return 1;
  }
  // Note: the .priv file was created with mode 0600 atomically (no umask window).
  std::printf("keygen: wrote %s and %s\n", privPath.c_str(), pubPath.c_str());
  std::printf("WARNING: %s is the crown jewels. Never ship it, never commit it,\n", privPath.c_str());
  std::printf("never copy it off your offline machine. Anyone holding it can sign\n");
  std::printf("mods as YOU.\n");
  return 0;
}

int doSign(const char* keyPath, const char* modDir) {
  if (!keyPath || !modDir) return usage();
  std::array<uint8_t, 32> seed;
  std::string err;
  if (!litholattice::crypto::readKeyFile(keyPath, seed, &err)) {
    std::printf("sign: %s\n", err.c_str());
    return 1;
  }
  if (!litholattice::crypto::signMod(modDir, seed.data(), &err)) {
    std::printf("sign: %s\n", err.c_str());
    return 1;
  }
  std::printf("sign: wrote %s/signature.json\n", modDir);
  return 0;
}

int doVerify(const char* keyPath, const char* modDir) {
  if (!keyPath || !modDir) return usage();
  std::array<uint8_t, 32> pub;
  std::string err;
  if (!litholattice::crypto::readKeyFile(keyPath, pub, &err)) {
    std::printf("verify: %s\n", err.c_str());
    return 1;
  }
  if (!litholattice::crypto::verifyMod(modDir, pub.data(), &err)) {
    std::printf("verify: FAIL: %s\n", err.c_str());
    return 1;
  }
  std::printf("verify: OK: %s signature valid\n", modDir);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  if (cmd == "keygen") return doKeygen(flag(argc, argv, "--out"));
  if (cmd == "sign") return doSign(flag(argc, argv, "--key"), flag(argc, argv, "--mod"));
  if (cmd == "verify") return doVerify(flag(argc, argv, "--key"), flag(argc, argv, "--mod"));
  return usage();
}
