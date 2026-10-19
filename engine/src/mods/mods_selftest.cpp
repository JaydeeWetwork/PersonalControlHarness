// mods selftest: IExtensible facet, dotted identity, depth cap, mod API
// network-confinement audit, mod API UI/input-confinement audit, and per-mod
// save-data integrity.
#include "litholattice/mods/mods_selftest.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "litholattice/mods/extensible_interface.h"
#include "litholattice/mods/mod_interface.h"  // LITHOLATTICE_SDK_VERSION
#include "litholattice/mods/extension_loader.h"
#include "litholattice/mods/extension_registry.h"
#include "litholattice/mods/extension_budget.h"
#include "litholattice/mods/extension_runtime.h"
#include "litholattice/mods/mod_loader.h"
#include "litholattice/mods/mod_permissions.h"
#include "litholattice/mods/mod_save_data.h"

namespace litholattice::mods {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] mods: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

// ---- fixture "extensible mods" (v1 static linking shape) ----

// A mod that IS extensible: exports the query function returning its facet.
extern "C" {
namespace fixture {
const LithoLatticeExtensionDecl kDecls[] = {
    {sizeof(LithoLatticeExtensionDecl), "brushes", ">=1.0.0", 25},
    {sizeof(LithoLatticeExtensionDecl), "masks", "^1.2.0", 10},
};

const LithoLatticeExtensionApi kChildApi = {
    sizeof(LithoLatticeExtensionApi),
    LITHOLATTICE_SDK_VERSION,
    nullptr,  // log: fixture leaves it null; the query path is what we test
    nullptr,
    nullptr,
};

const LithoLatticeExtensible kFacet = {
    sizeof(LithoLatticeExtensible),
    2,
    kDecls,
    &kChildApi,
    0,        // firstPartyGrantCount: no grants (default-deny)
    nullptr,  // firstPartyGrants
};

const LithoLatticeExtensible* LithoLattice_QueryExtensible(void) { return &kFacet; }

}  // namespace fixture
}  // extern "C"

void testFacetQuery(Checker& c) {
  // Non-extensible mod: null/absent query pointer -> null facet.
  c(LithoLatticeExtensibleQuery(nullptr) == nullptr,
    "facet: null query pointer (non-extensible mod) resolves to null");
  // Extensible mod: the exported query function returns its facet.
  const LithoLatticeExtensible* facet = LithoLatticeExtensibleQuery(fixture::LithoLattice_QueryExtensible);
  c(facet != nullptr, "facet: extensible mod's query function returns its facet");
  c(facet && facet->size == sizeof(LithoLatticeExtensible) && facet->extensionCount == 2 &&
        facet->extensions != nullptr && facet->childApi != nullptr,
    "facet: size-first struct, 2 extension decls, child API surface present");
  c(facet && std::string(facet->extensions[0].id) == "brushes" &&
        std::string(facet->extensions[0].versionRange) == ">=1.0.0" &&
        facet->extensions[0].budgetShare == 25 &&
        std::string(facet->extensions[1].id) == "masks" &&
        facet->extensions[1].budgetShare == 10,
    "facet: extension declarations carry id, version range, budget share");
  c(facet && facet->childApi->size == sizeof(LithoLatticeExtensionApi) &&
        facet->childApi->sdkVersion == LITHOLATTICE_SDK_VERSION,
    "facet: child API surface is size-first and stamped with the SDK version");
  c(std::string(LITHOLATTICE_EXTENSIBLE_QUERY_SYMBOL) == "LithoLattice_QueryExtensible",
    "facet: DLL-era symbol name is the exported query function name");
}

void testIdentity(Checker& c) {
  c(ExtensionRegistry::joinPath("lithokraft.worldedit", "brushes") == "lithokraft.worldedit.brushes",
    "identity: dotted path formation");
  c(ExtensionRegistry::joinPath("", "lithokraft") == "lithokraft",
    "identity: top-level mod has no leading dot");
  const std::vector<std::string> parts =
      ExtensionRegistry::splitPath("lithokraft.worldedit.brushes");
  c(parts.size() == 3 && parts[0] == "lithokraft" && parts[1] == "worldedit" && parts[2] == "brushes",
    "identity: dotted path parsing round-trips");
  c(ExtensionRegistry::splitPath("lithokraft..brushes").empty() &&
        ExtensionRegistry::splitPath("").empty() &&
        ExtensionRegistry::splitPath("Bad-Id.brushes").empty(),
    "identity: empty segments, empty paths, and bad ids are rejected");
  c(ExtensionRegistry::isValidId("brushes") && ExtensionRegistry::isValidId("a1_") &&
        !ExtensionRegistry::isValidId("") && !ExtensionRegistry::isValidId("Bad-Id") &&
        !ExtensionRegistry::isValidId(std::string(65, 'a')),
    "identity: ids are [a-z0-9_]{1,64}");
  c(ExtensionRegistry::parentPath("lithokraft.worldedit.brushes") == "lithokraft.worldedit" &&
        ExtensionRegistry::parentPath("lithokraft") == "",
    "identity: parent path derivation");
}

void testTreeAndDepthCap(Checker& c) {
  c(kDefaultMaxExtensionDepth == 8, "depth: default cap is the k-prefix constant 8");
  ExtensionRegistry reg;
  c(reg.maxExtensionDepth() == 8, "depth: registry starts at the default cap");
  c(reg.registerMod("lithokraft") && !reg.registerMod("lithokraft") &&
        !reg.registerMod("Bad-Id"),
    "tree: top-level mod registers once; bad ids and duplicates refused");
  c(reg.registerExtension("nope", "x").empty(), "tree: unknown parent refused");

  // Build a chain 8 deep: lithokraft.l1.l2...l8 (each +1 depth).
  std::string parent = "lithokraft";
  bool chainOk = true;
  for (int i = 1; i <= 8; ++i) {
    const std::string id = "l" + std::to_string(i);
    const std::string path = reg.registerExtension(parent, id);
    const std::string want = parent + "." + id;
    uint32_t depth = 0;
    chainOk = chainOk && path == want && reg.depthOf(path, depth) && depth == (uint32_t)i;
    parent = path;
  }
  c(chainOk, "tree: chain registers to depth 8 with correct dotted paths");
  c(reg.registerExtension(parent, "l9").empty(),
    "depth: 9th nesting level is rejected at the default cap of 8");
  uint32_t depth = 0;
  c(reg.depthOf("lithokraft.l1.l2", depth) && depth == 2, "tree: depthOf reports nesting depth");
  const ExtensionNode* node = reg.find("lithokraft.l1");
  c(node && node->parentPath == "lithokraft" && node->depth == 1,
    "tree: parent/child links are tracked");
  const std::vector<std::string> kids = reg.childrenOf("lithokraft");
  c(kids.size() == 1 && kids[0] == "lithokraft.l1", "tree: childrenOf lists direct children");
  c(reg.registerExtension("lithokraft", "l1").empty(),
    "tree: duplicate dotted paths are refused");
}

void testDepthOverride(Checker& c) {
  ExtensionRegistry reg;
  reg.setMaxExtensionDepth(16);
  c(reg.maxExtensionDepth() == 16, "depth: setMaxExtensionDepth raises the cap");
  std::string parent = "root";
  reg.registerMod("root");
  bool ok = true;
  for (int i = 1; i <= 16; ++i) {
    const std::string id = "d" + std::to_string(i);
    parent = reg.registerExtension(parent, id);
    ok = ok && !parent.empty();
  }
  c(ok, "depth: 16-deep chain registers after raising the cap");
  c(reg.registerExtension(parent, "d17").empty(), "depth: 17th level refused at cap 16");
  reg.setMaxExtensionDepth(0);
  c(reg.maxExtensionDepth() == kDefaultMaxExtensionDepth,
    "depth: setMaxExtensionDepth(0) resets to the default cap");
}

// ---- mod API surface audit (network confinement, layer 1) ----
//
// AUDIT TABLE — keep in sync with mod_interface.h. Every field of
// LithoLatticeModApi and LithoLatticeMod, in declaration order. The
// structural pins below fail if either struct grows without this table (and
// the audit) being updated: no new field — especially no network-adjacent
// one — can slip in unreviewed.
constexpr const char* kAuditedModApiFields[] = {
    "size",          "sdkMajor",      "registerBlock", "getBlock",
    "setBlock",      "log",           "worldTick",     "registerUiPanel",
    "registerInputAction", "loadAsset", "engineVersion", "sdkVersionFn",
    "protocolVersion", "netSend",
};
constexpr const char* kAuditedModFields[] = {
    "size",           "name",           "version",       "permissions",
    "isFirstParty",   "onLoad",         "onRegister",    "onWorldStart",
    "onWorldStop",    "onUnload",       "onTick",        "onClientFrame",
    "onDrawSolid",    "onDrawTranslucent", "onInputEvent", "onCamera",
    "onEntityVisible", "onInventoryChanged", "onNetMessage",
};

// Substrings that must never appear in a mod API surface name: socket
// primitives, fds, addresses, DNS, process control. Case-insensitive.
bool hasForbiddenToken(const char* name) {
  static const char* kForbidden[] = {
      "socket",  "sendto",   "recvfrom", "sendmsg",   "recvmsg",  "connect",
      "bind",    "listen",   "accept",   "getsock",   "setsock",  "getaddrinfo",
      "gethost", "inet_",    "poll",     "epoll",     "kqueue",   "ioctl",
      "dup",     "pipe",     "fork",     "exec",      "system",   "popen",
      "fopen",   "open(",    "_fd",      "fd_",       "packet",
      "datagram",
      // NOTE: "raw" is deliberately NOT a token: it false-positives on
      // "onDrawSolid" ("draw" contains "raw"). A "raw socket" style name
      // still trips the "socket" token above.
  };
  char lower[128];
  size_t n = std::strlen(name);
  if (n >= sizeof(lower)) return true;  // absurdly long name: fail closed
  for (size_t i = 0; i <= n; ++i)
    lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
  for (const char* tok : kForbidden)
    if (std::strstr(lower, tok) != nullptr) return true;
  return false;
}

// Substrings that must never appear in a mod API surface name: raw drawing /
// renderer access (UI confinement, item 5) and raw input-device / injection
// access (input confinement, item 6). Case-insensitive. Curated to avoid
// false positives on the legitimate entries: "draw" is deliberately NOT a
// token (onDrawSolid/onDrawTranslucent are engine->mod render callbacks,
// Half-Life style, and carry no painter), and neither are "ui", "panel",
// "input", "action", or "event" (the registerUiPanel / registerInputAction /
// onInputEvent entry points, which are strings-in/void-out by shape).
bool hasForbiddenUiInputToken(const char* name) {
  static const char* kForbidden[] = {
      // UI confinement: raw draw calls, renderer handles, widget handles.
      "painter", "renderer", "iuirenderer", "canvas", "drawcall", "opengl",
      "vulkan", "directx", "widget", "paint", "fillrect", "drawtext",
      // Input confinement: raw devices, polling, event injection.
      "keystroke", "keyboard", "mouse", "gamepad", "device", "poll",
      "backend", "inject", "synthesize", "keystate", "scancode",
  };
  char lower[128];
  size_t n = std::strlen(name);
  if (n >= sizeof(lower)) return true;  // absurdly long name: fail closed
  for (size_t i = 0; i <= n; ++i)
    lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
  for (const char* tok : kForbidden)
    if (std::strstr(lower, tok) != nullptr) return true;
  return false;
}

void testModApiNetAudit(Checker& c) {
  // Structural pins: the audited field tables cover the ENTIRE structs.
  // netSend is the last API field; onNetMessage the last mod field.
  LithoLatticeModApi api{};
  c(offsetof(LithoLatticeModApi, netSend) + sizeof(api.netSend) == sizeof(LithoLatticeModApi),
    "audit: netSend is the last LithoLatticeModApi field (struct matches the audit table)");
  LithoLatticeMod mod{};
  c(offsetof(LithoLatticeMod, onNetMessage) + sizeof(mod.onNetMessage) == sizeof(LithoLatticeMod),
    "audit: onNetMessage is the last LithoLatticeMod field (struct matches the audit table)");
  c(sizeof(kAuditedModApiFields) / sizeof(kAuditedModApiFields[0]) == 14,
    "audit: API field table has all 14 audited fields");
  c(sizeof(kAuditedModFields) / sizeof(kAuditedModFields[0]) == 19,
    "audit: mod field table has all 19 audited fields");

  // The actual audit: no field name carries a socket/fd/DNS/process token.
  bool clean = true;
  for (const char* f : kAuditedModApiFields) clean = clean && !hasForbiddenToken(f);
  for (const char* f : kAuditedModFields) clean = clean && !hasForbiddenToken(f);
  c(clean, "audit: no mod API surface name exposes socket/fd/DNS/process primitives");

  // The only network entry points are the two documented, engine-gated ones.
  bool netOnly = true;
  for (const char* f : kAuditedModApiFields) {
    const bool isNet = std::strcmp(f, "netSend") == 0;
    netOnly = netOnly && (isNet || std::strstr(f, "net") == nullptr);
  }
  c(netOnly, "audit: netSend is the sole network entry point in the API surface");
  c((kPermNet & 0xFFu) == 0 && kPermNet == (1u << 8),
    "audit: kPermNet is a fresh bit (1<<8), colliding with no existing permission");
}

// ---- mod API surface audit (UI confinement + input confinement, layer 1) ----
//
// AUDIT: no mod API surface name may expose raw drawing / renderer access or
// raw input-device / injection access. The tables kAuditedModApiFields /
// kAuditedModFields cover the ENTIRE structs (structurally pinned by
// testModApiNetAudit above), so every field below was declared in
// mod_interface.h and is reviewed here. Manual review (this is the audit;
// the token scan is the automated pin): registerUiPanel(mod, id, title) and
// registerInputAction(mod, id, displayName) are strings-in/void-out — they
// hand out no Painter*, no IUiRenderer*, no Widget*, no paint hook; the
// onInputEvent(mod, key, down) hook carries no Text codepoints and no device
// id. Enforcement lives in ui/mod_ui.h (ModUiFactory/ModPanel) and
// input/mod_input.h (ModInputRouter).
void testModApiUiInputAudit(Checker& c) {
  bool clean = true;
  for (const char* f : kAuditedModApiFields) clean = clean && !hasForbiddenUiInputToken(f);
  for (const char* f : kAuditedModFields) clean = clean && !hasForbiddenUiInputToken(f);
  c(clean, "audit: no mod API surface name exposes raw draw/input-device/injection primitives");

  // Exactly the three documented UI/input entry points exist: a new UI/input
  // field cannot slip in without this audit (and the confinement code)
  // being updated.
  size_t uiInputFields = 0;
  for (const char* f : kAuditedModApiFields)
    if (std::strstr(f, "Ui") != nullptr || std::strstr(f, "Input") != nullptr) ++uiInputFields;
  for (const char* f : kAuditedModFields)
    if (std::strstr(f, "Input") != nullptr) ++uiInputFields;
  c(uiInputFields == 3,
    "audit: exactly 3 UI/input API fields (registerUiPanel, registerInputAction, onInputEvent)");
}

// ---- per-mod save-data integrity ----

namespace {
namespace fs = std::filesystem;

fs::path freshTempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

void corruptOneByte(const fs::path& p) {
  std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
  // Flip a byte in the middle of the framed payload (past the header).
  f.seekp(0, std::ios::end);
  const auto len = f.tellp();
  f.seekp(len / 2);
  char b = 0;
  f.seekg(len / 2);
  f.get(b);
  b = static_cast<char>(b ^ 0xFF);
  f.seekp(len / 2);
  f.put(b);
}

const ModSaveResult* findResult(const std::vector<ModSaveResult>& v, const char* id) {
  for (const auto& r : v)
    if (r.modId == id) return &r;
  return nullptr;
}
}  // namespace

void testModSaveData(Checker& c) {
  // Version parsing.
  uint32_t maj = 0, min = 0, pat = 0;
  c(parseModSaveVersion("1.2.0", maj, min, pat) && maj == 1 && min == 2 && pat == 0,
    "modsave: semver parses");
  c(!parseModSaveVersion("1.2", maj, min, pat) && !parseModSaveVersion("a.b.c", maj, min, pat) &&
        !parseModSaveVersion("", maj, min, pat) && !parseModSaveVersion("1.2.0.4", maj, min, pat),
    "modsave: non-semver versions are rejected");

  // Round-trip: save then load verifies id + version + payload.
  {
    const fs::path dir = freshTempDir("ll_modsave_rt");
    ModSaveRegistry reg(dir);
    std::string err;
    const std::vector<uint8_t> payload = {9, 8, 7, 6, 5};
    c(reg.save("testmod", "1.2.0", 3, payload, &err),
      "modsave: save writes a section");
    const std::vector<ModSaveResult> rs = reg.loadAll({{"testmod", "1.2.0"}});
    const ModSaveResult* r = findResult(rs, "testmod");
    c(r && r->status == ModSaveStatus::Ok && !r->disabled() && r->payload == payload &&
          r->storedVersion == "1.2.0" && reg.quarantine().count() == 0,
      "modsave: round-trip verifies (Ok, payload + version intact, nothing quarantined)");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // Fresh install: mod present, no section -> Fresh with empty payload.
  {
    const fs::path dir = freshTempDir("ll_modsave_fresh");
    ModSaveRegistry reg(dir);
    const std::vector<ModSaveResult> rs = reg.loadAll({{"newmod", "0.1.0"}});
    const ModSaveResult* r = findResult(rs, "newmod");
    c(r && r->status == ModSaveStatus::Fresh && r->payload.empty() && !r->disabled(),
      "modsave: missing section is Fresh (empty data), not an error");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // ADVERSARIAL: corrupt a section -> quarantined, mod disabled, the world
  // still loads (the healthy mod's section verifies fine), file kept on disk.
  {
    const fs::path dir = freshTempDir("ll_modsave_corrupt");
    ModSaveRegistry reg(dir);
    std::string err;
    c(reg.save("goodmod", "1.0.0", 1, {1, 2, 3}, &err) &&
          reg.save("badmod", "1.0.0", 1, {4, 5, 6}, &err),
      "modsave: two sections written");
    corruptOneByte(reg.sectionPath("badmod"));
    const std::vector<ModSaveResult> rs =
        reg.loadAll({{"goodmod", "1.0.0"}, {"badmod", "1.0.0"}});
    const ModSaveResult* good = findResult(rs, "goodmod");
    const ModSaveResult* bad = findResult(rs, "badmod");
    c(bad && bad->status == ModSaveStatus::Corrupt && bad->disabled() && !bad->reason.empty(),
      "modsave: corrupt section -> Corrupt, mod disabled, reason recorded");
    c(good && good->status == ModSaveStatus::Ok && !good->disabled(),
      "modsave: corrupt mod data never kills the world (healthy mod still loads)");
    c(reg.quarantine().count() == 1 && fs::exists(reg.sectionPath("badmod")),
      "modsave: corrupt file quarantined AND kept on disk (never deleted)");
    // The engine must never overwrite a quarantined section.
    c(!reg.save("badmod", "1.0.0", 1, {7}, &err) && !err.empty(),
      "modsave: save refuses to overwrite a quarantined section (fail-closed)");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // ADVERSARIAL: inner id mismatch (file renamed / hand-placed) -> Corrupt.
  {
    const fs::path dir = freshTempDir("ll_modsave_idmismatch");
    ModSaveRegistry reg(dir);
    std::string err;
    c(reg.save("modx", "1.0.0", 1, {1}, &err), "modsave: section written for modx");
    std::error_code ec;
    fs::copy_file(reg.sectionPath("modx"), dir / "moddata" / "mody.llms", ec);
    const std::vector<ModSaveResult> rs = reg.loadAll({{"mody", "1.0.0"}});
    const ModSaveResult* r = findResult(rs, "mody");
    c(!ec && r && r->status == ModSaveStatus::Corrupt && r->disabled(),
      "modsave: section whose inner id mismatches the filename is Corrupt");
    fs::remove_all(dir, ec);
  }

  // Major version change -> VersionIncompatible, quarantined, mod disabled.
  {
    const fs::path dir = freshTempDir("ll_modsave_major");
    ModSaveRegistry reg(dir);
    std::string err;
    c(reg.save("vmod", "1.0.0", 1, {1, 2}, &err), "modsave: section written at 1.0.0");
    const std::vector<ModSaveResult> rs = reg.loadAll({{"vmod", "2.0.0"}});
    const ModSaveResult* r = findResult(rs, "vmod");
    c(r && r->status == ModSaveStatus::VersionIncompatible && r->disabled() &&
          !r->reason.empty() && reg.quarantine().count() == 1,
      "modsave: major version change quarantines the section and disables the mod");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // Minor change with no migration registered -> VersionIncompatible.
  {
    const fs::path dir = freshTempDir("ll_modsave_nomh");
    ModSaveRegistry reg(dir);
    std::string err;
    c(reg.save("mmod", "1.2.0", 7, {1, 2, 3}, &err), "modsave: section written at 1.2.0");
    const std::vector<ModSaveResult> rs = reg.loadAll({{"mmod", "1.3.0"}});
    const ModSaveResult* r = findResult(rs, "mmod");
    c(r && r->status == ModSaveStatus::VersionIncompatible && r->disabled(),
      "modsave: minor change with no migration hook quarantines (fail-closed)");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // Minor change WITH a migration hook -> migrated payload, mod runs.
  {
    const fs::path dir = freshTempDir("ll_modsave_migrate");
    ModSaveRegistry reg(dir);
    std::string err;
    c(reg.save("mmod", "1.2.0", 7, {1, 2, 3}, &err), "modsave: section written at 1.2.0");
    bool hookRan = false;
    reg.setMigrateFn([&](const std::string& id, const std::string& from,
                         const std::string& to, uint16_t fromData,
                         const std::vector<uint8_t>& payload) -> std::optional<std::vector<uint8_t>> {
      hookRan = true;
      if (id != "mmod" || from != "1.2.0" || to != "1.3.0" || fromData != 7) return std::nullopt;
      std::vector<uint8_t> out = payload;
      out.push_back(0xFF);  // schema upgrade marker
      return out;
    });
    const std::vector<ModSaveResult> rs = reg.loadAll({{"mmod", "1.3.0"}});
    const ModSaveResult* r = findResult(rs, "mmod");
    c(hookRan && r && r->status == ModSaveStatus::Ok && !r->disabled() &&
          r->payload == std::vector<uint8_t>({1, 2, 3, 0xFF}) && reg.quarantine().count() == 0,
      "modsave: migration hook upgrades the payload (mod runs, nothing quarantined)");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // Migration hook declining -> VersionIncompatible.
  {
    const fs::path dir = freshTempDir("ll_modsave_declined");
    ModSaveRegistry reg(dir);
    std::string err;
    c(reg.save("mmod", "1.2.0", 7, {1}, &err), "modsave: section written at 1.2.0");
    reg.setMigrateFn([](const std::string&, const std::string&, const std::string&, uint16_t,
                        const std::vector<uint8_t>&) -> std::optional<std::vector<uint8_t>> {
      return std::nullopt;  // cannot migrate: decline
    });
    const std::vector<ModSaveResult> rs = reg.loadAll({{"mmod", "1.3.0"}});
    const ModSaveResult* r = findResult(rs, "mmod");
    c(r && r->status == ModSaveStatus::VersionIncompatible && r->disabled(),
      "modsave: declined migration quarantines the section (fail-closed)");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // Orphaned section: mod removed -> kept on disk, never deleted, logged.
  {
    const fs::path dir = freshTempDir("ll_modsave_orphan");
    ModSaveRegistry reg(dir);
    std::string err;
    c(reg.save("gone", "1.0.0", 1, {9}, &err), "modsave: section written for gone");
    const std::vector<ModSaveResult> rs = reg.loadAll({});
    const ModSaveResult* r = findResult(rs, "gone");
    c(r && r->status == ModSaveStatus::Orphaned && !r->disabled() && fs::exists(reg.sectionPath("gone")),
      "modsave: orphaned section is kept on disk (never deleted), mod absent");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // Invalid mod id -> save refused (path safety).
  {
    const fs::path dir = freshTempDir("ll_modsave_badid");
    ModSaveRegistry reg(dir);
    std::string err;
    c(!reg.save("Bad-Id", "1.0.0", 1, {1}, &err) && !err.empty() &&
          !reg.save("../escape", "1.0.0", 1, {1}, &err),
      "modsave: invalid mod ids are refused before touching the filesystem");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
}

// ---- ExtensionLoader: recursive loading, scoping, privilege, unload ----

// A fake extensible parent for loader tests. Owns all strings the facet
// points at; the facet stays valid as long as the FakeParent lives.
struct FakeParent {
  // decls: {id, versionRange, budgetShare}; grants: ids elevated to first-party.
  FakeParent(std::vector<std::tuple<std::string, std::string, uint32_t>> decls,
             std::vector<std::string> grants = {}) {
    // Reserve first: decls_ stores c_str() pointers into ids_/ranges_, so the
    // vectors must not reallocate after the pointers are taken.
    ids_.reserve(decls.size());
    ranges_.reserve(decls.size());
    grantIds_.reserve(grants.size());
    for (const auto& [id, range, share] : decls) {
      ids_.push_back(id);
      ranges_.push_back(range.empty() ? "*" : range);
      decls_.push_back({sizeof(LithoLatticeExtensionDecl), ids_.back().c_str(),
                        ranges_.back().c_str(), share});
    }
    for (const auto& g : grants) {
      grantIds_.push_back(g);
      grantPtrs_.push_back(grantIds_.back().c_str());
    }
    childApi_.size = sizeof(LithoLatticeExtensionApi);
    childApi_.sdkVersion = LITHOLATTICE_SDK_VERSION;
    childApi_.log = +[](const char*) {};
    childApi_.engineVersion = +[]() -> uint32_t { return 0x00010000; };
    childApi_.sdkVersionFn = +[]() -> uint32_t { return LITHOLATTICE_SDK_VERSION; };
    facet_.size = sizeof(LithoLatticeExtensible);
    facet_.extensionCount = static_cast<uint32_t>(decls_.size());
    facet_.extensions = decls_.empty() ? nullptr : decls_.data();
    facet_.childApi = &childApi_;
    facet_.firstPartyGrantCount = static_cast<uint32_t>(grantPtrs_.size());
    facet_.firstPartyGrants = grantPtrs_.empty() ? nullptr : grantPtrs_.data();
  }
  const LithoLatticeExtensible* facet() const { return &facet_; }
  const LithoLatticeExtensionApi* childApi() const { return &childApi_; }

 private:
  std::vector<std::string> ids_, ranges_, grantIds_;
  std::vector<LithoLatticeExtensionDecl> decls_;
  std::vector<const char*> grantPtrs_;
  LithoLatticeExtensionApi childApi_{};
  LithoLatticeExtensible facet_{};
};

// One query function per fixture slot. The loader only ever sees plain
// function pointers (v1 static linking); each test parks its FakeParent
// facets in g_slots before loading.
const LithoLatticeExtensible* g_slots[16] = {nullptr};
template <int I>
const LithoLatticeExtensible* slotQuery() {
  return g_slots[I];
}
LithoLatticeQueryExtensibleFn slotFn(int i) {
  switch (i) {
    case 0: return &slotQuery<0>;
    case 1: return &slotQuery<1>;
    case 2: return &slotQuery<2>;
    case 3: return &slotQuery<3>;
    case 4: return &slotQuery<4>;
    case 5: return &slotQuery<5>;
    case 6: return &slotQuery<6>;
    case 7: return &slotQuery<7>;
    case 8: return &slotQuery<8>;
    case 9: return &slotQuery<9>;
    case 10: return &slotQuery<10>;
    case 11: return &slotQuery<11>;
    case 12: return &slotQuery<12>;
    case 13: return &slotQuery<13>;
    case 14: return &slotQuery<14>;
    case 15: return &slotQuery<15>;
    default: return nullptr;
  }
}

void regParent(ExtensionLoader& loader, int slot, const FakeParent& parent,
               const char* id, const char* version,
               const LithoLatticeMod* imod = nullptr, bool firstParty = false) {
  g_slots[slot] = parent.facet();
  loader.registerStaticModule(
      ExtensionLoader::StaticModule(id, version, slotFn(slot), imod, firstParty));
}

void regLeaf(ExtensionLoader& loader, const char* id, const char* version,
             const LithoLatticeMod* imod = nullptr) {
  loader.registerStaticModule(
      ExtensionLoader::StaticModule(id, version, nullptr, imod));
}

void testExtensionLoaderNesting(Checker& c) {
  FakeParent root({{"a", ">=1.0.0", 25}});
  FakeParent pa({{"b", "*", 10}});
  FakeParent pb({{"cc", "=1.0.0", 0}});
  ExtensionLoader loader;
  regParent(loader, 0, root, "root", "1.0.0");
  regParent(loader, 1, pa, "a", "1.2.0");
  regParent(loader, 2, pb, "b", "2.0.0");
  regLeaf(loader, "cc", "1.0.0");
  std::string err;
  c(loader.loadMod("root", err), "loader: 3-deep nesting loads (root.a.b.cc)");
  c(err.empty(), "loader: clean load sets no error");
  c(loader.loadMod("root", err), "loader: loading twice is an idempotent no-op");
  const ExtensionContext* ca = loader.contextFor("root.a");
  const ExtensionContext* cb = loader.contextFor("root.a.b");
  const ExtensionContext* cc = loader.contextFor("root.a.b.cc");
  c(ca && cb && cc, "loader: every level gets an ExtensionContext");
  uint32_t d = 0;
  c(ca->depth == 1 && cb->depth == 2 && cc->depth == 3 &&
        loader.registry().depthOf("root.a.b.cc", d) && d == 3,
    "loader: depths are 1/2/3 under the top-level mod");
  c(ca->childApi == root.childApi() && cb->childApi == pa.childApi() &&
        cc->childApi == pb.childApi(),
    "loader: each context carries its own parent's filtered childApi");
  c(ca->budgetShare == 25 && cb->budgetShare == 10 && cc->budgetShare == 0,
    "loader: budget shares are carried per declaration");
  c(ca->parentPath == "root" && cb->parentPath == "root.a" &&
        cc->parentPath == "root.a.b",
    "loader: contexts link to their parent path only");
  c(loader.contextFor("root") == nullptr && loader.contextFor("nope") == nullptr,
    "loader: top-level mods and unknown paths get no extension context");
  const std::vector<std::string> wantLoad = {"root", "root.a", "root.a.b", "root.a.b.cc"};
  c(loader.lastLoadOrder() == wantLoad, "loader: load order is parent-before-children");
}

void testExtensionLoaderDepthCap(Checker& c) {
  // Exactly 8 deep (caproot.d1...d8): the 8th level sits exactly at the cap.
  {
    ExtensionLoader loader;
    std::vector<std::string> ids = {"caproot"};
    for (int i = 1; i <= 8; ++i) ids.push_back("d" + std::to_string(i));
    std::vector<FakeParent> mods;
    for (int i = 0; i < 8; ++i)
      mods.emplace_back(FakeParent{{{ids[i + 1], "*", 0}}});
    for (int i = 0; i < 8; ++i) regParent(loader, i, mods[i], ids[i].c_str(), "1.0.0");
    regLeaf(loader, ids[8].c_str(), "1.0.0");
    std::string err;
    c(loader.loadMod("caproot", err), "loader: 8-deep chain loads at the default cap");
    uint32_t depth = 0;
    c(loader.registry().depthOf("caproot.d1.d2.d3.d4.d5.d6.d7.d8", depth) && depth == 8,
      "loader: 8th level sits at depth 8");
  }
  // 9 deep: the 9th level is refused, fail-closed with rollback.
  {
    ExtensionLoader loader;
    std::vector<std::string> ids = {"caproot9"};
    for (int i = 1; i <= 9; ++i) ids.push_back("e" + std::to_string(i));
    std::vector<FakeParent> mods;
    for (int i = 0; i < 9; ++i)
      mods.emplace_back(FakeParent{{{ids[i + 1], "*", 0}}});
    for (int i = 0; i < 9; ++i) regParent(loader, i, mods[i], ids[i].c_str(), "1.0.0");
    regLeaf(loader, ids[9].c_str(), "1.0.0");
    std::string err;
    c(!loader.loadMod("caproot9", err), "loader: 9th nesting level is rejected");
    c(err.find("depth") != std::string::npos,
      "loader: the refusal names the depth cap");
    c(!loader.registry().contains("caproot9") &&
          !loader.registry().contains("caproot9.e1") &&
          loader.contextFor("caproot9.e1.e2") == nullptr &&
          loader.lastLoadOrder().empty(),
      "loader: failed load rolls back every registered node (fail-closed)");
  }
}

void testExtensionLoaderCycle(Checker& c) {
  // Self-declaration: "selfish" declares "selfish" as its extension.
  {
    FakeParent p({{"selfish", "*", 0}});
    ExtensionLoader loader;
    regParent(loader, 0, p, "selfish", "1.0.0");
    std::string err;
    c(!loader.loadMod("selfish", err), "loader: self-declaration is rejected");
    c(err.find("cycle") != std::string::npos,
      "loader: self-declaration error names the cycle");
    c(!loader.registry().contains("selfish"), "loader: rejected load leaves no trace");
  }
  // Ancestor cycle: cyc_a -> cyc_b -> cyc_a.
  {
    FakeParent pa({{"cyc_b", "*", 0}});
    FakeParent pb({{"cyc_a", "*", 0}});
    ExtensionLoader loader;
    regParent(loader, 1, pa, "cyc_a", "1.0.0");
    regParent(loader, 2, pb, "cyc_b", "1.0.0");
    std::string err;
    c(!loader.loadMod("cyc_a", err), "loader: ancestor cycle (a->b->a) is rejected");
    c(err.find("cycle") != std::string::npos,
      "loader: ancestor-cycle error names the cycle");
  }
  // Diamond is NOT a cycle: droot hosts m1 and m2; both host dleaf.
  {
    FakeParent r({{"m1", "*", 0}, {"m2", "*", 0}});
    FakeParent m1({{"dleaf", "*", 0}});
    FakeParent m2({{"dleaf", "*", 0}});
    ExtensionLoader loader;
    regParent(loader, 3, r, "droot", "1.0.0");
    regParent(loader, 4, m1, "m1", "1.0.0");
    regParent(loader, 5, m2, "m2", "1.0.0");
    regLeaf(loader, "dleaf", "1.0.0");
    std::string err;
    c(loader.loadMod("droot", err),
      "loader: diamond (one module under two parents) is not a cycle");
    c(loader.contextFor("droot.m1.dleaf") && loader.contextFor("droot.m2.dleaf"),
      "loader: the shared module loads once per parent path");
  }
}

void testExtensionVersionRange(Checker& c) {
  FakeParent vr({{"newkid", ">=2.0.0", 0}});
  FakeParent vr2({{"oldkid", "^1.0.0", 0}});
  ExtensionLoader loader;
  regParent(loader, 6, vr, "vroot", "1.0.0");
  regParent(loader, 7, vr2, "vroot2", "1.0.0");
  regLeaf(loader, "newkid", "1.0.0");  // too old for >=2.0.0
  regLeaf(loader, "oldkid", "1.4.2");  // satisfies ^1.0.0
  std::string err;
  c(!loader.loadMod("vroot", err), "loader: version-range mismatch is refused");
  c(err.find("2.0.0") != std::string::npos,
    "loader: the refusal names the unsatisfied range");
  c(!loader.registry().contains("vroot"), "loader: range failure rolls back");
  c(loader.loadMod("vroot2", err), "loader: satisfying range (^1.0.0 vs 1.4.2) loads");
}

// AUDIT TABLE — keep in sync with extension_loader.h. Every ExtensionContext
// field, in declaration order. childApi is last: the structural pin below
// fails if the struct ever grows a member after it, so no full-API pointer
// can be appended without this audit (and the review) being updated.
constexpr const char* kAuditedContextFields[] = {
    "path", "parentPath", "depth", "isFirstParty", "budgetShare", "childApi",
};

void testExtensionContextIsolation(Checker& c) {
  ExtensionContext probe{};
  c(offsetof(ExtensionContext, childApi) + sizeof(probe.childApi) ==
        sizeof(ExtensionContext),
    "isolation: childApi is the last ExtensionContext member (no full-API "
    "pointer can hide in the context)");
  c(sizeof(kAuditedContextFields) / sizeof(kAuditedContextFields[0]) == 6,
    "isolation: context field table covers all 6 audited fields");

  // Runtime: the child context carries the parent's filtered childApi — and
  // there is no member, accessor, or path by which an extension could reach
  // the full LithoLatticeModApi or a sibling extension.
  FakeParent iroot({{"kid", "*", 0}, {"sib", "*", 0}});
  ExtensionLoader loader;
  regParent(loader, 8, iroot, "iroot", "1.0.0");
  regLeaf(loader, "kid", "1.0.0");
  regLeaf(loader, "sib", "1.0.0");
  std::string err;
  c(loader.loadMod("iroot", err), "isolation: fixture loads");
  const ExtensionContext* kc = loader.contextFor("iroot.kid");
  c(kc && kc->childApi == iroot.childApi(),
    "isolation: extension context carries the parent's FILTERED childApi "
    "(pointer-identical, not a copy of the full API)");
  c(kc && kc->childApi->log && kc->childApi->engineVersion && kc->childApi->sdkVersionFn,
    "isolation: filtered surface exposes exactly log + version queries");
  c(kc && kc->path == "iroot.kid" && kc->parentPath == "iroot",
    "isolation: the context names the extension and its parent — and nothing "
    "else; sibling 'iroot.sib' is unreachable from it");
}

void testExtensionPrivilege(Checker& c) {
  // First-party parent "fproot" grants "vip" only. "pleb" must stay
  // third-party (no inheritance); vip's own child must stay third-party
  // (grants are not transitive).
  FakeParent vipP({{"vkid", "*", 0}});
  FakeParent fproot({{"vip", "*", 0}, {"pleb", "*", 0}}, {"vip"});
  ExtensionLoader loader;
  regParent(loader, 9, fproot, "fproot", "1.0.0", nullptr, true);
  regParent(loader, 10, vipP, "vip", "1.0.0");
  regLeaf(loader, "pleb", "1.0.0");
  regLeaf(loader, "vkid", "1.0.0");
  std::string err;
  c(loader.loadMod("fproot", err), "privilege: fixture loads");
  const ExtensionContext* vip = loader.contextFor("fproot.vip");
  const ExtensionContext* pleb = loader.contextFor("fproot.pleb");
  const ExtensionContext* vkid = loader.contextFor("fproot.vip.vkid");
  c(vip && vip->isFirstParty, "privilege: explicitly granted extension is first-party");
  c(pleb && !pleb->isFirstParty,
    "privilege: ungranted extension of a FIRST-PARTY parent stays third-party "
    "(isFirstParty is never inherited)");
  c(vkid && !vkid->isFirstParty,
    "privilege: grants are not transitive (granted extension's child stays "
    "third-party)");
  // Default-deny on pre-grant facets: the v1.0 worker-1 fixture grants nothing.
  c(LithoLatticeExtensibleGrantsFirstParty(fixture::LithoLattice_QueryExtensible(),
                                           "brushes") == 0,
    "privilege: v1.0 facet (no grants listed) grants nothing — default deny");
  // Size gate: even a facet that claims a grant is denied when its size
  // predates the grant tail.
  LithoLatticeExtensible old = *fixture::LithoLattice_QueryExtensible();
  old.size = (uint32_t)offsetof(LithoLatticeExtensible, firstPartyGrantCount);
  c(LithoLatticeExtensibleGrantsFirstParty(&old, "brushes") == 0,
    "privilege: size gate denies grants on pre-v1.1 facets");
}

namespace {
int g_unloadCalls = 0;
void countingOnUnload(LithoLatticeModHandle) { ++g_unloadCalls; }
}  // namespace

void testCascadeUnload(Checker& c) {
  FakeParent uroot({{"ua", "*", 0}});
  FakeParent ua({{"ub", "*", 0}});
  ExtensionLoader loader;
  LithoLatticeMod imod{};
  imod.size = sizeof(LithoLatticeMod);
  imod.onUnload = countingOnUnload;
  regParent(loader, 11, uroot, "uroot", "1.0.0", &imod);
  regParent(loader, 12, ua, "ua", "1.0.0", &imod);
  regLeaf(loader, "ub", "1.0.0", &imod);
  std::string err;
  g_unloadCalls = 0;
  c(loader.loadMod("uroot", err), "unload: fixture loads");
  loader.unloadMod("uroot");
  const std::vector<std::string> want = {"uroot.ua.ub", "uroot.ua", "uroot"};
  c(loader.lastUnloadOrder() == want,
    "unload: children unload before the parent (reverse depth order)");
  c(g_unloadCalls == 3, "unload: onUnload ran for every unloaded module");
  c(!loader.registry().contains("uroot") && !loader.registry().contains("uroot.ua") &&
        !loader.registry().contains("uroot.ua.ub") &&
        loader.contextFor("uroot.ua") == nullptr,
    "unload: registry nodes and contexts are cleared");
  // Save-data contract: unload never deletes data (documented in
  // extension_loader.h); unknown ids are a no-op.
  loader.unloadMod("nope");
  c(loader.lastUnloadOrder().empty(), "unload: unknown id is a no-op");
}

void testModJsonExtensible(Checker& c) {
  const fs::path dir = freshTempDir("ll_modjson_ext");
  std::error_code ec;
  fs::create_directories(dir / "wedit", ec);
  {
    std::ofstream out(dir / "wedit" / "mod.json", std::ios::binary);
    out << R"({"id":"wedit","version":"1.0.0","sdk":"1","type":"content",)"
           R"("extensible":true,)"
           R"("extensions":[{"id":"brushes","version":">=1.0.0","budgetShare":25},)"
           R"({"id":"Bad-Id","version":"*"},)"
           R"({"id":"masks","budgetShare":200}]})";
  }
  fs::create_directories(dir / "plain", ec);
  {
    std::ofstream out(dir / "plain" / "mod.json", std::ios::binary);
    out << R"({"id":"plain","version":"1.0.0","sdk":"1"})";
  }
  ModLoader ml;
  const std::vector<ModInfo> mods = ml.scan(dir.string());
  const ModInfo* wedit = nullptr;
  const ModInfo* plain = nullptr;
  for (const auto& m : mods) {
    if (m.id == "wedit") wedit = &m;
    if (m.id == "plain") plain = &m;
  }
  c(wedit && wedit->extensible, "modjson: 'extensible': true parses into ModInfo");
  bool brushesOk = false, masksOk = false;
  if (wedit) {
    c(wedit->extensions.size() == 2,
      "modjson: valid extension decls parse; bad ids are skipped");
    for (const auto& e : wedit->extensions) {
      if (e.id == "brushes" && e.versionRange == ">=1.0.0" && e.budgetShare == 25)
        brushesOk = true;
      if (e.id == "masks" && e.versionRange == "*" && e.budgetShare == 100) masksOk = true;
    }
  }
  c(brushesOk, "modjson: extension id/versionRange/budgetShare parse");
  c(masksOk, "modjson: budgetShare > 100 is clamped to 100 (fail-safe)");
  c(plain && !plain->extensible && plain->extensions.empty(),
    "modjson: absent 'extensible'/'extensions' default to false/empty");
  fs::remove_all(dir, ec);
}

// REGRESSION (audit 2026-10-11): std::stoul threw std::out_of_range on huge
// numeric strings in "sdk" ("99999999999999999999"), uncaught — a single
// malformed mod.json crashed the engine at startup. Now fail-closed: the mod
// is refused, the scan continues.
void testModJsonHugeNumbers(Checker& c) {
  const fs::path dir = freshTempDir("ll_modjson_huge");
  std::error_code ec;
  fs::create_directories(dir / "hugesdk", ec);
  {
    std::ofstream out(dir / "hugesdk" / "mod.json", std::ios::binary);
    out << R"({"id":"hugesdk","version":"1.0.0","sdk":"99999999999999999999","type":"script"})";
  }
  fs::create_directories(dir / "hugesdkrange", ec);
  {
    // Huge number inside an sdk RANGE bound (the parseSemver path).
    std::ofstream out(dir / "hugesdkrange" / "mod.json", std::ios::binary);
    out << R"({"id":"hugesdkrange","version":"1.0.0","sdk":">=99999999999999999999.0.0","type":"script"})";
  }
  fs::create_directories(dir / "wrapsdk", ec);
  {
    // 2^32: stoul does NOT throw on 64-bit (it narrows) — the old code would
    // silently wrap this to sdk 0. Refused now.
    std::ofstream out(dir / "wrapsdk" / "mod.json", std::ios::binary);
    out << R"({"id":"wrapsdk","version":"1.0.0","sdk":"4294967296","type":"script"})";
  }
  fs::create_directories(dir / "good", ec);
  {
    std::ofstream out(dir / "good" / "mod.json", std::ios::binary);
    out << R"({"id":"good","version":"1.2.3","sdk":"1","type":"script"})";
  }
  ModLoader ml;
  // Must not throw: this is the regression (the old code crashed here).
  std::vector<ModInfo> mods;
  bool threw = false;
  try {
    mods = ml.scan(dir.string());
  } catch (...) {
    threw = true;
  }
  c(!threw, "modjson: huge sdk numbers do not crash the scan");
  bool goodFound = false, badFound = false;
  for (const auto& m : mods) {
    if (m.id == "good") goodFound = true;
    if (m.id == "hugesdk" || m.id == "hugesdkrange" || m.id == "wrapsdk") badFound = true;
  }
  c(goodFound && !badFound,
    "modjson: malformed-sdk mods are refused fail-closed; the good mod still scans");
  fs::remove_all(dir, ec);
}

}  // namespace

// ---- worker 3: sibling-share validation, hierarchical budgets, crash isolation ----

// Sibling shares summing over 100 are warn-and-clamped (proportional), never
// a load refusal: budget oversubscription is a config error, not a security
// violation. Uses slots 14/15 (0-13 are taken by the tests above).
void testExtensionShareSumClamp(Checker& c) {
  // 60 + 60 = 120 -> 50 / 50.
  {
    FakeParent greedy({{"a", "*", 60}, {"b", "*", 60}});
    ExtensionLoader loader;
    regParent(loader, 15, greedy, "greedy", "1.0.0");
    regLeaf(loader, "a", "1.0.0");
    regLeaf(loader, "b", "1.0.0");
    std::string err;
    c(loader.loadMod("greedy", err),
      "shares: oversubscribed tree still loads (warn-and-clamp, not refusal)");
    const ExtensionContext* ca = loader.contextFor("greedy.a");
    const ExtensionContext* cb = loader.contextFor("greedy.b");
    c(ca && cb && ca->budgetShare == 50 && cb->budgetShare == 50,
      "shares: 60+60 clamps proportionally to 50/50 (sum <= 100)");
  }
  // 70 + 40 = 110 -> floor(70*100/110) = 63, floor(40*100/110) = 36.
  {
    FakeParent uneven({{"x", "*", 70}, {"y", "*", 40}});
    ExtensionLoader loader;
    regParent(loader, 15, uneven, "uneven", "1.0.0");
    regLeaf(loader, "x", "1.0.0");
    regLeaf(loader, "y", "1.0.0");
    std::string err;
    c(loader.loadMod("uneven", err), "shares: uneven oversubscription loads");
    const ExtensionContext* cx = loader.contextFor("uneven.x");
    const ExtensionContext* cy = loader.contextFor("uneven.y");
    c(cx && cy && cx->budgetShare == 63 && cy->budgetShare == 36,
      "shares: 70+40 clamps to 63/36 (proportional floor, sum 99 <= 100)");
  }
  // Sums <= 100 pass through untouched, at nested levels too.
  {
    FakeParent root({{"p", "*", 100}});
    FakeParent pa({{"m", "*", 25}, {"n", "*", 10}});
    ExtensionLoader loader;
    regParent(loader, 15, root, "okroot", "1.0.0");
    regParent(loader, 14, pa, "p", "1.0.0");
    regLeaf(loader, "m", "1.0.0");
    regLeaf(loader, "n", "1.0.0");
    std::string err;
    c(loader.loadMod("okroot", err), "shares: fitting sums load");
    const ExtensionContext* cp = loader.contextFor("okroot.p");
    const ExtensionContext* cm = loader.contextFor("okroot.p.m");
    const ExtensionContext* cn = loader.contextFor("okroot.p.n");
    c(cp && cp->budgetShare == 100 && cm && cm->budgetShare == 25 &&
          cn && cn->budgetShare == 10,
      "shares: sums <= 100 keep their declared values at every level");
  }
}

namespace budgetfix {
// A deliberately crashing extension: throws across its IMod boundary.
int g_healthyTicks = 0;
int g_crashyUnloads = 0;
void healthyTick(LithoLatticeModHandle, LithoLatticeWorldHandle) { ++g_healthyTicks; }
void crashyTick(LithoLatticeModHandle, LithoLatticeWorldHandle) {
  throw std::runtime_error("deliberate test crash");
}
void crashyUnload(LithoLatticeModHandle) { ++g_crashyUnloads; }
LithoLatticeMod makeMod(void (*tick)(LithoLatticeModHandle, LithoLatticeWorldHandle),
                        void (*unload)(LithoLatticeModHandle)) {
  LithoLatticeMod m{};
  m.size = sizeof(LithoLatticeMod);
  m.onTick = tick;
  m.onUnload = unload;
  return m;
}
}  // namespace budgetfix

void testExtensionBudgetHierarchy(Checker& c) {
  FakeParent root({{"big", "*", 25}, {"small", "*", 10}, {"free", "*", 0}});
  ExtensionLoader loader;
  regParent(loader, 13, root, "budgettest", "1.0.0");
  regLeaf(loader, "big", "1.0.0");
  regLeaf(loader, "small", "1.0.0");
  regLeaf(loader, "free", "1.0.0");
  std::string err;
  c(loader.loadMod("budgettest", err), "budget: tree loads");
  ExtensionRuntime rt(loader);
  rt.sync();
  c(rt.budgetNodeCount() == 4, "budget: one node per loaded mod/extension");

  const ExtensionBudget* mod = rt.budgetFor("budgettest");
  ExtensionBudget* big = rt.budgetFor("budgettest.big");
  ExtensionBudget* small = rt.budgetFor("budgettest.small");
  const ExtensionBudget* free = rt.budgetFor("budgettest.free");
  c(mod && big && small && free, "budget: every node gets a budget");
  c(mod->tickQuotaUs() == kDefaultModTickBudgetUs,
    "budget: top-level mod gets the default 1024us tick budget");
  c(big->tickQuotaUs() == 256 && small->tickQuotaUs() == 102 &&
        free->tickQuotaUs() == 0,
    "budget: child quota = parent quota * share / 100 (256 / 102 / 0)");
  c(small->tickQuotaUs() <= big->tickQuotaUs() &&
        big->tickQuotaUs() <= mod->tickQuotaUs(),
    "budget: a child quota never exceeds its parent's share");
  c(big->memoryQuotaBytes() == kDefaultModMemoryBudgetBytes * 25 / 100,
    "budget: memory quota is carved from the parent's the same way");
  c(big->gate() == BudgetGate::kAllow, "budget: fresh nodes tick normally");

  // Over-quota extension: 4 consecutive overruns -> throttled. Siblings and
  // the parent are independent nodes and stay allowed.
  bool bigRan = false;
  for (int i = 0; i < 4; ++i) big->recordUsage(100000);  // 100ms >> 256us quota
  c(big->gate() == BudgetGate::kThrottled && big->throttleCount() == 1,
    "budget: 4 consecutive overruns throttle the over-quota extension");
  c(small->gate() == BudgetGate::kAllow && mod->gate() == BudgetGate::kAllow,
    "budget: throttling one extension leaves parent and siblings unaffected");
  c(rt.tick("budgettest.big", [&] { bigRan = true; }) ==
        ExtensionRuntime::TickOutcome::kThrottled &&
        !bigRan,
    "budget: a throttled extension's tick is skipped, not run");
  c(rt.tick("budgettest.small", [] {}) == ExtensionRuntime::TickOutcome::kTicked,
    "budget: the sibling still ticks while the over-quota one is throttled");

  // After the cooldown the node resumes (on probation); a short new streak
  // then suspends it. The rest of the tree is still unaffected.
  for (uint32_t i = 0; i < kBudgetThrottleCooldownTicks; ++i) rt.advanceTick();
  c(big->gate() == BudgetGate::kAllow,
    "budget: throttle cooldown expires and the node resumes");
  big->recordUsage(100000);
  big->recordUsage(100000);
  c(big->gate() == BudgetGate::kSuspended && !big->suspendReason().empty(),
    "budget: sustained overruns after probation suspend with a logged reason");
  c(rt.tick("budgettest.big", [&] { bigRan = true; }) ==
        ExtensionRuntime::TickOutcome::kSuspended &&
        !bigRan,
    "budget: a suspended extension never runs again until reload");
  c(small->gate() == BudgetGate::kAllow && mod->gate() == BudgetGate::kAllow,
    "budget: suspending one extension leaves parent and siblings unaffected");

  // Memory overage feeds the same state machine (accounting-based in v1).
  small->noteMemoryBytes(static_cast<int64_t>(kDefaultModMemoryBudgetBytes));
  small->recordUsage(0);  // no CPU usage at all: the memory overage alone counts
  c(small->overrunStreak() == 1,
    "budget: a memory-quota overage counts as a budget overrun");
  small->noteMemoryBytes(-static_cast<int64_t>(kDefaultModMemoryBudgetBytes));
}

// A deliberately crashing test extension: its onTick throws. The runtime must
// contain it — onUnload runs exactly once, its subtree unloads, and the
// parent plus the healthy sibling keep ticking. (v1 fault domains catch C++
// exceptions; hard crashes like SIGSEGV need the DLL-era process boundary.)
void testExtensionCrashIsolation(Checker& c) {
  budgetfix::g_healthyTicks = 0;
  budgetfix::g_crashyUnloads = 0;
  FakeParent root({{"crashy", "*", 25}, {"healthy", "*", 25}});
  static LithoLatticeMod crashyMod =
      budgetfix::makeMod(budgetfix::crashyTick, budgetfix::crashyUnload);
  static LithoLatticeMod healthyMod = budgetfix::makeMod(budgetfix::healthyTick, nullptr);
  ExtensionLoader loader;
  regParent(loader, 13, root, "crashtest", "1.0.0");
  regLeaf(loader, "crashy", "1.0.0", &crashyMod);
  regLeaf(loader, "healthy", "1.0.0", &healthyMod);
  std::string err;
  c(loader.loadMod("crashtest", err),
    "crashiso: parent + crashing child + healthy sibling load");

  ExtensionRuntime rt(loader);
  rt.sync();
  // The parent ticks fine before the crash.
  c(rt.tick("crashtest", [] {}) == ExtensionRuntime::TickOutcome::kTicked,
    "crashiso: parent ticks before the crash");

  // The crash.
  const ExtensionRuntime::TickOutcome r = rt.tick(
      "crashtest.crashy", [] { budgetfix::crashyTick(nullptr, nullptr); });
  c(r == ExtensionRuntime::TickOutcome::kCrashed,
    "crashiso: a throwing onTick is contained (kCrashed), not propagated");
  c(budgetfix::g_crashyUnloads == 1,
    "crashiso: the crashed extension's onUnload ran exactly once");
  c(!loader.registry().contains("crashtest.crashy") &&
        loader.contextFor("crashtest.crashy") == nullptr,
    "crashiso: the crashed extension's subtree is unloaded");
  c(rt.crashed("crashtest.crashy"),
    "crashiso: the crash is recorded as diagnostic history");

  // Parent and sibling survive: they are still loaded and still tick.
  c(loader.registry().contains("crashtest") &&
        loader.registry().contains("crashtest.healthy") &&
        loader.contextFor("crashtest.healthy") != nullptr,
    "crashiso: parent and healthy sibling survive the crash");
  c(rt.tick("crashtest", [] {}) == ExtensionRuntime::TickOutcome::kTicked,
    "crashiso: the parent keeps ticking after the crash");
  c(rt.tick("crashtest.healthy",
            [] { budgetfix::healthyTick(nullptr, nullptr); }) ==
            ExtensionRuntime::TickOutcome::kTicked &&
        budgetfix::g_healthyTicks == 1,
    "crashiso: the healthy sibling keeps ticking after the crash");
  c(rt.tick("crashtest.crashy", [] {}) == ExtensionRuntime::TickOutcome::kUnknown,
    "crashiso: ticking the unloaded crashed path is kUnknown, not fatal");

  // Reloading the same identity clears the crash mark (fresh state).
  loader.unloadMod("crashtest");
  regParent(loader, 13, root, "crashtest", "1.0.0");
  regLeaf(loader, "crashy", "1.0.0", &crashyMod);
  regLeaf(loader, "healthy", "1.0.0", &healthyMod);
  c(loader.loadMod("crashtest", err), "crashiso: crashed tree reloads cleanly");
  rt.sync();
  c(!rt.crashed("crashtest.crashy"),
    "crashiso: reloading clears the crash mark");
}

// ---- permission manifest schema + approval flow (item 7) ----

// REGRESSION: parsePermissions used to be an if/else chain that never parsed
// "net", so Worker B's kPermNet could not be granted via mod.json.
void testPermissionNetRegression(Checker& c) {
  const fs::path dir = freshTempDir("ll_perm_net");
  std::error_code ec;
  fs::create_directories(dir / "netmod", ec);
  {
    std::ofstream out(dir / "netmod" / "mod.json", std::ios::binary);
    out << R"({"id":"netmod","version":"1.0.0","sdk":"1","type":"script",)"
           R"("permissions":["blocks","net","bogus_perm"]})";
  }
  ModLoader ml;
  const std::vector<ModInfo> mods = ml.scan(dir.string());
  const ModInfo* m = mods.empty() ? nullptr : &mods[0];
  c(mods.size() == 1, "perm: netmod scans");
  c(m && (m->permissions & kPermNet) != 0,
    "perm: \"net\" parses to kPermNet (regression)");
  c(m && (m->permissions & kPermBlocks) != 0, "perm: \"blocks\" still parses");
  c(m && m->permissions == uint32_t(kPermBlocks | kPermNet),
    "perm: unknown permission names are ignored and never become bits");
  fs::remove_all(dir, ec);
}

// REGRESSION (audit 2026-10-11): kPermPilotSenses existed in mod_interface.h
// and MCP's permBitName() but was missing from the manifest schema (kNameBits
// / kAllPermissionBits), so no mod could ever declare "pilot_senses" and
// resolve() masked it out.
void testPermissionPilotSensesRegression(Checker& c) {
  c(mods::permissionBitForName("pilot_senses") == uint32_t(kPermPilotSenses),
    "perm: \"pilot_senses\" parses to kPermPilotSenses (regression)");
  c(mods::permissionNameForBit(kPermPilotSenses) != nullptr &&
        std::string(mods::permissionNameForBit(kPermPilotSenses)) == "pilot_senses",
    "perm: kPermPilotSenses round-trips through the name table");
  c((kAllPermissionBits & uint32_t(kPermPilotSenses)) != 0,
    "perm: kPermPilotSenses is in kAllPermissionBits (resolve() no longer masks it out)");
  const fs::path dir = freshTempDir("ll_perm_senses");
  std::error_code ec;
  fs::create_directories(dir / "sensemod", ec);
  {
    std::ofstream out(dir / "sensemod" / "mod.json", std::ios::binary);
    out << R"({"id":"sensemod","version":"1.0.0","sdk":"1","type":"script",)"
           R"("permissions":["pilot_senses"]})";
  }
  ModLoader ml;
  const std::vector<ModInfo> mods = ml.scan(dir.string());
  const ModInfo* m = mods.empty() ? nullptr : &mods[0];
  c(m && m->permissions == uint32_t(kPermPilotSenses),
    "perm: \"pilot_senses\" grants end-to-end via mod.json");
  fs::remove_all(dir, ec);
}

void testPermissionManifestParse(Checker& c) {
  const fs::path dir = freshTempDir("ll_perm_manifest");
  std::error_code ec;
  fs::create_directories(dir / "fullmod", ec);
  {
    std::ofstream out(dir / "fullmod" / "mod.json", std::ios::binary);
    out << R"({"id":"fullmod","version":"1.0.0","sdk":"1","type":"script",)"
           R"("permissionSet":1,)"
           R"("permissions":["blocks","world_write","net"],)"
           R"("permissionRationales":{"blocks":"Adds 40 building blocks",)"
           R"json("net":"Waypoint sync (engine-gated channel only)","bogus":"ignored"},)json"
           R"("criticalPermissions":["world_write","tick"],)"
           R"("firstParty":true})";
  }
  ModLoader ml;
  const std::vector<ModInfo> mods = ml.scan(dir.string());
  const ModInfo* m = mods.empty() ? nullptr : &mods[0];
  c(m && m->permissionSetVersion == 1, "perm: permissionSet parses");
  c(m && m->permissionRationales.size() == 2 &&
        m->permissionRationales.at("blocks") == "Adds 40 building blocks" &&
        m->permissionRationales.at("net") == "Waypoint sync (engine-gated channel only)",
    "perm: per-permission rationales parse; rationale for an unknown "
    "permission is ignored");
  c(m && m->criticalPermissions == uint32_t(kPermWorldWrite),
    "perm: criticalPermissions masks to requested bits ('tick' dropped)");
  c(m && m->firstPartyClaim, "perm: firstParty claim parses");
  fs::remove_all(dir, ec);
}

namespace {
ModInfo mkPermMod(const char* id, const char* type, uint32_t perms) {
  ModInfo m;
  m.id = id;
  m.version = "1.0.0";
  m.type = type;
  m.permissions = perms;
  m.permissionRationales["net"] = "Waypoint sync";
  return m;
}
}  // namespace

void testPermissionApprovalFlow(Checker& c) {
  // 1. Headless deny default: denyAll strips every unapproved permission; the
  // mod still loads (reduced), never silently elevated.
  {
    PermissionStore store;
    ModPermissionGate gate;
    const PermissionResolution r = gate.resolve(
        mkPermMod("a", "script", kPermBlocks | kPermNet | kPermTick), store,
        denyAllPermissions);
    c(r.action == ModLoadAction::kLoadStripped && r.granted == 0 &&
          r.stripped == uint32_t(kPermBlocks | kPermNet | kPermTick) &&
          r.denied == r.stripped && !r.reason.empty(),
      "perm: headless deny-all strips every unapproved permission (mod loads "
      "reduced)");
  }
  // 2. An approving prompt grants exactly the approved subset; rationales
  // reach the request items.
  {
    PermissionStore store;
    ModPermissionGate gate;
    bool sawRationale = false;
    PermissionPromptFn ui = [&](const PermissionRequest& req) {
      c(req.modId == "b" && req.items.size() == 3,
        "perm: prompt sees one item per requested permission");
      for (const auto& it : req.items)
        if (it.name == "net") sawRationale = (it.rationale == "Waypoint sync");
      return uint32_t(kPermBlocks | kPermNet);  // approve blocks+net, deny tick
    };
    const PermissionResolution r = gate.resolve(
        mkPermMod("b", "script", kPermBlocks | kPermNet | kPermTick), store, ui);
    c(r.action == ModLoadAction::kLoadStripped &&
          r.granted == uint32_t(kPermBlocks | kPermNet) && r.denied == kPermTick &&
          sawRationale,
      "perm: approving prompt grants the approved subset; rationale reaches "
      "the request");
  }
  // 3. A denied load-critical permission refuses the load.
  {
    PermissionStore store;
    ModPermissionGate gate;
    ModInfo m = mkPermMod("c", "script", kPermWorldWrite);
    m.criticalPermissions = kPermWorldWrite;
    const PermissionResolution r = gate.resolve(m, store, denyAllPermissions);
    c(r.action == ModLoadAction::kRefuse && r.granted == 0 && !r.reason.empty(),
      "perm: denied load-critical permission refuses the load");
  }
  // 4. Content mods auto-approve the content-safe subset (blocks, assets)
  // with no prompt; net still needs approval.
  {
    PermissionStore store;
    ModPermissionGate gate;
    bool promptRan = false;
    PermissionPromptFn ui = [&](const PermissionRequest& req) {
      promptRan = true;
      c(req.items.size() == 1 && req.items[0].name == "net",
        "perm: content-mod prompt sees only the non-content-safe bit");
      return uint32_t(0);
    };
    const PermissionResolution r = gate.resolve(
        mkPermMod("d", "content", kPermBlocks | kPermAssets | kPermNet), store,
        ui);
    c(promptRan && r.granted == uint32_t(kPermBlocks | kPermAssets) &&
          r.stripped == kPermNet && r.action == ModLoadAction::kLoadStripped,
      "perm: content mod auto-approves blocks/assets; net stripped under "
      "deny default");
  }
  // 5. Verified first-party claim: all granted, prompt never consulted.
  {
    PermissionStore store;
    ModPermissionGate gate;
    gate.setFirstPartyIds({"lithokraft"});
    bool promptRan = false;
    PermissionPromptFn ui = [&](const PermissionRequest&) {
      promptRan = true;
      return uint32_t(0);
    };
    ModInfo m = mkPermMod("lithokraft", "native", kPermNet | kPermWorldWrite);
    m.firstPartyClaim = true;
    const PermissionResolution r = gate.resolve(m, store, ui);
    c(!promptRan && r.action == ModLoadAction::kLoad &&
          r.granted == uint32_t(kPermNet | kPermWorldWrite),
      "perm: verified first-party claim grants all; prompt not consulted");
  }
  // 6. Unverified firstParty claim: logged, treated as third-party.
  {
    PermissionStore store;
    ModPermissionGate gate;  // empty trusted list
    bool promptRan = false;
    PermissionPromptFn ui = [&](const PermissionRequest&) {
      promptRan = true;
      return uint32_t(0);
    };
    ModInfo m = mkPermMod("impostor", "script", kPermNet);
    m.firstPartyClaim = true;
    const PermissionResolution r = gate.resolve(m, store, ui);
    c(promptRan && r.granted == 0 && r.action == ModLoadAction::kLoadStripped,
      "perm: unverified firstParty claim is treated as third-party (no "
      "silent grant)");
  }
  // 7. The prompt cannot approve bits it was not asked about.
  {
    PermissionStore store;
    ModPermissionGate gate;
    PermissionPromptFn greedy = [](const PermissionRequest&) {
      return uint32_t(0xFFFFFFFF);
    };
    const PermissionResolution r =
        gate.resolve(mkPermMod("e", "script", kPermTick), store, greedy);
    c(r.granted == kPermTick,
      "perm: prompt approval is masked to the requested bits");
  }
}

void testPermissionUpdateRepompt(Checker& c) {
  PermissionStore store;  // in-memory across "launches"
  ModPermissionGate gate;
  auto mk = [](const char* ver, uint32_t perms) {
    ModInfo m;
    m.id = "upmod";
    m.version = ver;
    m.type = "script";
    m.permissions = perms;
    return m;
  };
  // v1.0: approve blocks, deny tick.
  PermissionPromptFn v1 = [](const PermissionRequest&) {
    return uint32_t(kPermBlocks);
  };
  const PermissionResolution r1 =
      gate.resolve(mk("1.0.0", kPermBlocks | kPermTick), store, v1);
  c(r1.granted == kPermBlocks && r1.denied == kPermTick,
    "perm: v1.0 grant recorded (blocks approved, tick denied)");

  // v1.1 adds net AND changes the version. Review F-M7: grants are bound
  // to (id, version), so a version change re-decides EVERYTHING — the
  // prompt sees all three bits, not just the new one, and neither the old
  // grant nor the old denial carries over on its own.
  int prompts = 0;
  PermissionPromptFn v2 = [&](const PermissionRequest& req) {
    ++prompts;
    c(req.items.size() == 3,
      "perm: version change re-prompts for ALL permissions (grants are "
      "bound to id+version, F-M7)");
    return uint32_t(kPermBlocks | kPermNet);  // approve blocks+net, deny tick again
  };
  const PermissionResolution r2 = gate.resolve(
      mk("1.1.0", kPermBlocks | kPermTick | kPermNet), store, v2);
  c(prompts == 1 && r2.granted == uint32_t(kPermBlocks | kPermNet) &&
        (r2.denied & kPermTick) != 0,
    "perm: version change re-decides all bits (blocks+net approved, tick "
    "denied again)");

  // Same version (still 1.1.0), same manifest: no prompt at all — within
  // one (id, version) the stored decision stands.
  prompts = 0;
  PermissionPromptFn silent = [&](const PermissionRequest&) {
    ++prompts;
    return uint32_t(0);
  };
  const PermissionResolution r3 = gate.resolve(
      mk("1.1.0", kPermBlocks | kPermTick | kPermNet), store, silent);
  c(prompts == 0 && r3.action == ModLoadAction::kLoadStripped &&
        r3.granted == uint32_t(kPermBlocks | kPermNet),
    "perm: unchanged manifest at the same version re-prompts for nothing");

  // Same version, drops tick from the manifest: no prompt; loads clean.
  prompts = 0;
  const PermissionResolution r4 =
      gate.resolve(mk("1.1.0", kPermBlocks | kPermNet), store, silent);
  c(prompts == 0 && r4.granted == uint32_t(kPermBlocks | kPermNet) &&
        r4.action == ModLoadAction::kLoad,
    "perm: dropping a denied permission needs no prompt");

  // Same version, manifest asks for one NEW bit (events): the prompt sees
  // ONLY that bit — the within-version update diff still applies.
  prompts = 0;
  PermissionPromptFn v5 = [&](const PermissionRequest& req) {
    ++prompts;
    c(req.items.size() == 1 && req.items[0].name == "events",
      "perm: same-version update re-prompts only for the NEW permission");
    return uint32_t(kPermEvents);
  };
  const PermissionResolution r5 = gate.resolve(
      mk("1.1.0", kPermBlocks | kPermNet | kPermEvents), store, v5);
  c(prompts == 1 &&
        r5.granted == uint32_t(kPermBlocks | kPermNet | kPermEvents),
    "perm: same-version new bit is prompted and granted, old grants stand");
}

void testPermissionStoreRoundTrip(Checker& c) {
  const fs::path dir = freshTempDir("ll_perm_store");
  std::error_code ec;
  const std::string path = (dir / "grants.json").string();
  {
    PermissionStore store(path);
    StoredGrant sg;
    sg.modVersion = "1.0.0";
    sg.permissionSetVersion = kPermissionSetVersion;
    sg.requested = kPermBlocks | kPermNet;
    sg.granted = kPermBlocks;
    sg.denied = kPermNet;
    store.record("rtmod", sg);
    c(store.save(), "perm: store saves");
  }
  {
    PermissionStore store(path);  // ctor loads from disk
    const StoredGrant* g = store.lookup("rtmod");
    c(g && g->modVersion == "1.0.0" && g->requested == uint32_t(kPermBlocks | kPermNet) &&
          g->granted == kPermBlocks && g->denied == kPermNet,
      "perm: grant record round-trips through the file");
    c(store.lookup("missing") == nullptr, "perm: unknown mod has no grant");
  }
  // Corrupt file: treated as empty, never fatal.
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "{nope";
  }
  {
    PermissionStore store(path);
    c(store.count() == 0,
      "perm: corrupt grants file is treated as empty (never fatal)");
  }
  fs::remove_all(dir, ec);
}

// ModLoader::load() runs the approval flow BEFORE the mod's onLoad: the
// effective mask rides on ModInfo, and refused mods never reach the binder.
void testPermissionLoadWiring(Checker& c) {
  const fs::path dir = freshTempDir("ll_perm_load");
  std::error_code ec;
  auto mkmod = [&](const char* name, const std::string& json) {
    fs::create_directories(dir / name, ec);
    std::ofstream(dir / name / "mod.json", std::ios::binary) << json;
  };
  mkmod("stripped",
        R"({"id":"stripped","version":"1.0.0","sdk":"1","type":"script",)"
        R"("permissions":["net","tick"]})");
  mkmod("refused",
        R"({"id":"refused","version":"1.0.0","sdk":"1","type":"script",)"
        R"("permissions":["world_write"],"criticalPermissions":["world_write"]})");
  mkmod("clean",
        R"({"id":"clean","version":"1.0.0","sdk":"1","type":"content",)"
        R"("permissions":["blocks"]})");
  PermissionStore store;  // in-memory: no file written
  ModPermissionGate gate;
  ModLoader ml;
  const std::vector<ModInfo> loaded =
      ml.load(dir.string(), store, gate, denyAllPermissions);
  const ModInfo* stripped = nullptr;
  const ModInfo* refused = nullptr;
  const ModInfo* clean = nullptr;
  for (const auto& m : loaded) {
    if (m.id == "stripped") stripped = &m;
    if (m.id == "refused") refused = &m;
    if (m.id == "clean") clean = &m;
  }
  c(stripped && stripped->loadAction == ModLoadAction::kLoadStripped &&
        stripped->grantedPermissions == 0,
    "perm: load() strips unapproved permissions (headless deny) but loads "
    "the mod");
  c(refused == nullptr,
    "perm: load() refuses a mod whose load-critical permission was denied");
  c(clean && clean->loadAction == ModLoadAction::kLoad &&
        clean->grantedPermissions == kPermBlocks,
    "perm: load() auto-approves content-safe permissions for content mods");
  fs::remove_all(dir, ec);
}

int runModsSelfTest() {
  Checker c;
  testFacetQuery(c);
  testIdentity(c);
  testTreeAndDepthCap(c);
  testDepthOverride(c);
  testModApiNetAudit(c);
  testModApiUiInputAudit(c);
  testModSaveData(c);
  testExtensionLoaderNesting(c);
  testExtensionLoaderDepthCap(c);
  testExtensionLoaderCycle(c);
  testExtensionVersionRange(c);
  testExtensionContextIsolation(c);
  testExtensionPrivilege(c);
  testCascadeUnload(c);
  testModJsonExtensible(c);
  testModJsonHugeNumbers(c);
  testExtensionShareSumClamp(c);
  testExtensionBudgetHierarchy(c);
  testExtensionCrashIsolation(c);
  testPermissionNetRegression(c);
  testPermissionPilotSensesRegression(c);
  testPermissionManifestParse(c);
  testPermissionApprovalFlow(c);
  testPermissionUpdateRepompt(c);
  testPermissionStoreRoundTrip(c);
  testPermissionLoadWiring(c);
  return c.fails();
}

}  // namespace litholattice::mods
