// crypto selftest: SHA-256/512 NIST vectors, Ed25519 RFC 8032 vectors,
// and mod-signing round-trips (sign/verify, tamper rejection, determinism).
#include "litholattice/crypto/crypto_selftest.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "litholattice/crypto/ed25519.h"
#include "litholattice/crypto/sha256.h"
#include "litholattice/crypto/sha512.h"
#include "litholattice/crypto/signing.h"

namespace litholattice::crypto {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] crypto: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

bool hexEq(const uint8_t* data, size_t len, const char* hex) {
  return toHex(data, len) == hex;
}

void testSha256(Checker& check) {
  // NIST vectors.
  check(hexEq(sha256(nullptr, 0).data(), 32,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        "sha256: empty string");
  const char* abc = "abc";
  check(hexEq(sha256(reinterpret_cast<const uint8_t*>(abc), 3).data(), 32,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "sha256: \"abc\"");
  const char* long1 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  check(hexEq(sha256(reinterpret_cast<const uint8_t*>(long1), std::strlen(long1)).data(), 32,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        "sha256: 448-bit message");
  // Incremental API matches one-shot.
  Sha256 s;
  s.update(reinterpret_cast<const uint8_t*>("a"), 1);
  s.update(reinterpret_cast<const uint8_t*>("b"), 1);
  s.update(reinterpret_cast<const uint8_t*>("c"), 1);
  check(hexEq(s.final().data(), 32,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "sha256: incremental update");
}

void testSha512(Checker& check) {
  check(hexEq(sha512(nullptr, 0).data(), 64,
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"),
        "sha512: empty string");
  const char* abc = "abc";
  check(hexEq(sha512(reinterpret_cast<const uint8_t*>(abc), 3).data(), 64,
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"),
        "sha512: \"abc\"");
}

void testEd25519(Checker& check) {
  // Known-answer vectors, cross-generated with libsodium (PyNaCl) 2026-10-12.
  // Deterministic seeds so the vectors are stable.
  struct Vec {
    const char* seed;
    const char* pub;
    const char* msg;
    const char* sig;
  };
  const Vec vecs[] = {
      {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
       "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8", "",
       "9ca53579530654d5c3df77089ef45eda613e2fedf670e96bedac4639504e584"
       "5ef4b95d5793077233dd16817b2532e9c5525872a73a4ad74b759369a9e05c102"},
      {"4242424242424242424242424242424242424242424242424242424242424242",
       "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12", "72",
       "50716e28ba667c177224a50918dc1bf616466b862e0cf71e9ccdcb8fc991877"
       "af16449337979a56711f767d076233d62ffe32febfa2fc43fab271a8bf592320a"},
      {"00070e151c232a31383f464d545b626970777e858c939aa1a8afb6bdc4cbd2d9",
       "4d0ec3a0d03fefff93a9bcce2a24b406d46f44aeff1f7787daedb13caafa402c", "61663832",
       "198072d5ba8a89abac1edcf67b98d10937e81aab44edb1f80e9867ee2bd0ea6"
       "22988300be7bb87893301dbb91156d3b9c559d809088a340b16a9bb23167b8e0f"},
  };
  for (int i = 0; i < 3; ++i) {
    const Vec& v = vecs[i];
    ed25519::Seed seed;
    if (!fromHex(v.seed, seed.data(), 32)) {
      check(false, "ed25519: test vector hex decode");
      continue;
    }
    ed25519::PublicKey pub;
    ed25519::SecretKey sec;
    ed25519::keypair_from_seed(seed, pub, sec);
    char what[64];
    std::snprintf(what, sizeof what, "ed25519: vector %d keypair matches", i + 1);
    check(toHex(pub.data(), 32) == v.pub, what);

    std::vector<uint8_t> msg;
    {
      size_t n = std::strlen(v.msg) / 2;
      msg.resize(n);
      if (!fromHex(v.msg, msg.data(), n)) {
        check(false, "ed25519: msg hex decode");
        continue;
      }
    }
    ed25519::Signature sig = ed25519::sign(msg.data(), msg.size(), sec);
    std::snprintf(what, sizeof what, "ed25519: vector %d signature matches", i + 1);
    check(toHex(sig.data(), 64) == v.sig, what);
    std::snprintf(what, sizeof what, "ed25519: vector %d verifies", i + 1);
    check(ed25519::verify(sig, msg.data(), msg.size(), pub), what);
  }
  // Tampered message must not verify.
  {
    ed25519::Seed seed;
    fromHex(vecs[1].seed, seed.data(), 32);
    ed25519::PublicKey pub;
    ed25519::SecretKey sec;
    ed25519::keypair_from_seed(seed, pub, sec);
    const uint8_t msg[] = {'r'};
    ed25519::Signature sig = ed25519::sign(msg, 1, sec);
    const uint8_t bad[] = {'s'};
    check(!ed25519::verify(sig, bad, 1, pub), "ed25519: tampered message rejected");
    // Flipped signature bit.
    ed25519::Signature badsig = sig;
    badsig[10] ^= 1;
    check(!ed25519::verify(badsig, msg, 1, pub), "ed25519: tampered signature rejected");
  }
}

void testSigning(Checker& check) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "litho_sign_selftest";
  fs::remove_all(dir);
  fs::create_directories(dir / "sub");

  auto writeFile = [&](const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
  };
  writeFile(dir / "mod.json", "{\"id\": \"testmod\"}");
  writeFile(dir / "sub" / "data.txt", "hello");
  writeFile(dir / "z.bin", std::string("\x00\x01\x02", 3));

  std::array<uint8_t, 32> seed, pub, otherPub;
  std::array<uint8_t, 32> otherSeed;
  check(generateKeypair(seed, pub), "signing: keygen works");
  check(generateKeypair(otherSeed, otherPub), "signing: second keygen works");

  std::string err;
  check(signMod(dir, seed.data(), &err), "signing: signMod writes signature.json: " + err);
  check(fs::exists(dir / "signature.json"), "signing: signature.json exists");

  // Deterministic: signing twice yields the identical file.
  std::ifstream f1(dir / "signature.json", std::ios::binary);
  const std::string sig1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
  f1.close();
  check(signMod(dir, seed.data(), &err), "signing: re-sign works: " + err);
  std::ifstream f2(dir / "signature.json", std::ios::binary);
  const std::string sig2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
  check(sig1 == sig2 && !sig1.empty(), "signing: deterministic (identical signature.json)");

  check(verifyMod(dir, pub.data(), &err), "signing: verify with correct key: " + err);
  check(!verifyMod(dir, otherPub.data(), &err), "signing: wrong key rejected");

  // Tamper a file -> verify fails.
  writeFile(dir / "sub" / "data.txt", "goodbye");
  check(!verifyMod(dir, pub.data(), &err), "signing: tampered file rejected");
  writeFile(dir / "sub" / "data.txt", "hello");  // restore
  check(verifyMod(dir, pub.data(), &err), "signing: restored file verifies: " + err);

  // Tamper the signature -> fails.
  {
    std::ifstream in(dir / "signature.json");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Flip a hex char in the signature field.
    size_t pos = text.find("\"signature\": \"");
    if (pos != std::string::npos) {
      pos += 14;
      text[pos] = (text[pos] == 'a' ? 'b' : 'a');
      std::ofstream out(dir / "signature.json", std::ios::binary | std::ios::trunc);
      out << text;
    }
  }
  check(!verifyMod(dir, pub.data(), &err), "signing: tampered signature rejected");

  // Key file round-trip.
  const fs::path kp = dir / "t.priv", kq = dir / "t.pub";
  check(writeKeyFile(kp, seed.data()) && writeKeyFile(kq, pub.data()), "signing: key files write");
  std::array<uint8_t, 32> r1, r2;
  check(readKeyFile(kp, r1) && r1 == seed, "signing: .priv round-trips");
  check(readKeyFile(kq, r2) && r2 == pub, "signing: .pub round-trips");

#if defined(__unix__) || defined(__APPLE__)
  // writePrivateKeyFile hardening (P3-3, Appendix E L2): a pre-existing
  // file with loose permissions is forced to 0600, and a symlink target
  // is refused (O_NOFOLLOW) rather than written through.
  {
    const fs::path pk = dir / "hardened.priv";
    check(writePrivateKeyFile(pk, seed.data()), "signing: writePrivateKeyFile creates the key file");
    std::error_code pec;
    fs::permissions(pk, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read, pec);
    check(writePrivateKeyFile(pk, seed.data()), "signing: writePrivateKeyFile rewrites an existing key file");
    const auto perms = fs::status(pk, pec).permissions();
    check(!pec && (perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
          "signing: pre-existing loose key file is forced to 0600 (E L2)");
    const fs::path target = dir / "elsewhere.priv";
    const fs::path link = dir / "link.priv";
    fs::create_symlink(target, link, pec);
    check(!pec && !writePrivateKeyFile(link, seed.data()) && !fs::exists(target),
          "signing: writePrivateKeyFile refuses to write through a symlink (E L2)");
  }
#endif

  fs::remove_all(dir);
}

}  // namespace

void testCanonicalHashFraming(Checker& check) {
  // Appendix E H1 regression (P1b): the v1 framing (rel ‖ 0x00 ‖ bytes, no
  // lengths, no count) made these two DIFFERENT mods hash identically, so
  // a signature for one transplanted to the other:
  //   mod A: file a = "", file b = ""        -> stream: a \0 b \0
  //   mod B: file a = bytes "b\0" (2 bytes)  -> stream: a \0 b \0
  // Under the v2 length-prefixed framing they MUST hash differently.
  namespace fs = std::filesystem;
  const fs::path base = fs::temp_directory_path() / "litho_hash_framing_selftest";
  fs::remove_all(base);
  const fs::path dirA = base / "modA", dirB = base / "modB";
  fs::create_directories(dirA);
  fs::create_directories(dirB);
  auto writeFile = [&](const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
  };
  writeFile(dirA / "a", "");
  writeFile(dirA / "b", "");
  writeFile(dirB / "a", std::string("b\0", 2));  // 'b' followed by a NUL byte

  std::string err;
  const auto hashA = canonicalModHash(dirA, &err);
  const auto hashB = canonicalModHash(dirB, &err);
  check(hashA.has_value() && hashB.has_value(), "framing: transplant pair both hash");
  check(hashA && hashB && *hashA != *hashB,
        "framing: Appendix E collision pair (A {a='',b=''} vs B {a='b\\0'}) hashes DIFFERENTLY");

  // A normal mod still hashes/signs/verifies round-trip with an ephemeral
  // keypair under the v2 framing, and a signature issued for mod A does NOT
  // verify against mod B's contents.
  std::array<uint8_t, 32> seed, pub;
  check(generateKeypair(seed, pub), "framing: ephemeral keygen works");
  check(signMod(dirA, seed.data(), &err), "framing: sign mod A: " + err);
  check(verifyMod(dirA, pub.data(), &err), "framing: mod A verifies round-trip: " + err);
  // Copy A's signature.json onto B: B must NOT verify with it.
  fs::copy_file(dirA / "signature.json", dirB / "signature.json",
                fs::copy_options::overwrite_existing);
  check(!verifyMod(dirB, pub.data(), &err),
        "framing: A's signature transplanted onto B is rejected");

  fs::remove_all(base);
}

int runCryptoSelfTest() {
  Checker check;
  testSha256(check);
  testSha512(check);
  testEd25519(check);
  testSigning(check);
  testCanonicalHashFraming(check);
  std::printf("crypto: %d failure(s)\n", check.fails());
  return check.fails();
}

}  // namespace litholattice::crypto
