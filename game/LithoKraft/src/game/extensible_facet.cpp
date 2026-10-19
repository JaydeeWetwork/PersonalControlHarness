// LithoKraft's IExtensible facet: the game is a host for extensions
// (sub-mods). v1 declares ZERO extensions — the point is the host contract
// exists and the engine may query it. The facet exports the canonical query
// symbol (LITHOLATTICE_EXTENSIBLE_QUERY_SYMBOL, "LithoLattice_QueryExtensible")
// so the DLL-era loader resolves it with dlsym; for v1 static linking the
// engine's mod boot path takes its address directly when it wires
// ExtensionLoader into the game's mod startup (builder seam: that wiring is
// not done yet — main.cpp's loadModDirectory only reads mod.json so far).
#include "litholattice/mods/extensible_interface.h"
#include "litholattice/mods/mod_interface.h"  // LITHOLATTICE_SDK_VERSION

#include "litholattice/core/log.h"

namespace {

void extLog(const char* msg) {
  litholattice::logInfo("lithokraft/ext: %s", msg ? msg : "");
}
uint32_t extEngineVersion() { return 0x00010000; }  // LithoKraft 0.1.0
uint32_t extSdkVersionFn() { return LITHOLATTICE_SDK_VERSION; }

// The parent's API surface for its future extensions: logging + version
// queries only (the v1 child API). Extensions never see the full
// LithoLatticeModApi — capability scoping is enforced by ExtensionLoader.
const LithoLatticeExtensionApi kLithoKraftChildApi = {
    sizeof(LithoLatticeExtensionApi),
    LITHOLATTICE_SDK_VERSION,
    &extLog,
    &extEngineVersion,
    &extSdkVersionFn,
};

const LithoLatticeExtensible kLithoKraftExtensible = {
    sizeof(LithoLatticeExtensible),
    0,  // extensionCount: no extensions declared initially
    nullptr,  // extensions
    &kLithoKraftChildApi,
    0,  // firstPartyGrantCount: default-deny; nothing granted yet
    nullptr,  // firstPartyGrants
};

}  // namespace

extern "C" const LithoLatticeExtensible* LithoLattice_QueryExtensible(void) {
  return &kLithoKraftExtensible;
}
