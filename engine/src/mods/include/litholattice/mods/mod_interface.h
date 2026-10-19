// LithoLattice mod interface v1: C-compatible, DLL-ready.
// Rule 1: No STL, exceptions, or RTTI across the boundary. Opaque handles,
// versioned structs (size first), plain C types. Static linking for v1, but
// designed as if DLLs are coming (Half-Life-SDK style).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LITHOLATTICE_SDK_VERSION 1

// Opaque handles (never dereferenced by mods).
typedef struct LlWorld* LithoLatticeWorldHandle;
typedef struct LithoLatticeMod* LithoLatticeModHandle;

// Versioned struct: size field first for forward compatibility.
typedef struct LithoLatticeModBlockDef {
  uint32_t size;           ///< sizeof(this struct) — engine checks version
  const char* id;          ///< "modname:block_name" (namespaced)
  const char* name;        ///< display name
  uint32_t flags;
  // ... shape, texture, etc.
} LithoLatticeModBlockDef;

// Permissions (mod.json). Content mods load silently; script/native prompt.
typedef enum LithoLatticeModPermission {
  kPermBlocks   = 1 << 0,  ///< register blocks/items
  kPermWorldRead  = 1 << 1,
  kPermWorldWrite = 1 << 2,  ///< setBlock (rate-limited)
  kPermTick       = 1 << 3,  ///< per-tick callback
  kPermEvents     = 1 << 4,  ///< block placed/broken hooks
  kPermUI         = 1 << 5,  ///< register UI panels
  kPermInput      = 1 << 6,  ///< register input actions
  kPermAssets     = 1 << 7,  ///< load textures/models/sounds
  kPermNet        = 1 << 8,  ///< engine-gated network API (netSend/onNetMessage).
                             ///  The ONLY network path a mod may use: no sockets,
                             ///  no fds, no addresses, no DNS. Rate-limited per
                             ///  mod (bytes/sec + packets/sec); overruns are
                             ///  throttled, sustained abuse suspends access.
                             ///  See net/confinement.h for the enforcement.
  kPermConsole    = 1 << 9,  ///< console surface: register console commands (namespaced
                             ///  "modid:..."), read/write cvars through the console
                             ///  registry, and drive the console language from
                             ///  scripts (consoleCmd / consoleCvar / consoleSetCvar
                             ///  bindings, allowlist-gated on this bit). Does NOT
                             ///  grant file access: exec/saveArchived stay sandboxed
                             ///  to the engine's config dir regardless.
  kPermPlayerControl = 1 << 10, ///< MCP player control: lets an approved agent
                             ///  pilot the local player through the REAL input
                             ///  path (synthetic InputEvents merged where the
                             ///  keyboard enters). Tick-aligned, rate-limited,
                             ///  deny-by-default. The model plays by the same
                             ///  rules as a keyboard player: same physics, same
                             ///  reach checks, same sim-thread marshaling for
                             ///  world edits. Does NOT grant world_set_block or
                             ///  console: those need their own bits.
  kPermPilotSenses = 1 << 11, ///< MCP pilot senses: lets an approved agent
                             ///  SEE and HEAR through a piloted player
                             ///  (vision.capture_frame + audio.capture_chunk).
                             ///  Observation only: grants NO actuation — the
                             ///  model cannot move, break, or place anything
                             ///  with this bit alone (that is kPermPlayerControl).
                             ///  Per-pilot rate-limited, deny-by-default.
} LithoLatticeModPermission;

// The API surface. Mods get a pointer to this; they call in, engine never
// calls out except through the IMod lifecycle below.
typedef struct LithoLatticeModApi {
  uint32_t size;      ///< sizeof(this struct)
  uint32_t sdkMajor;  ///< SDK major line this API struct was built against
                      ///  (LITHOLATTICE_SDK_VERSION; renamed from sdkVersion:
                      ///  the name collided with the sdkVersion() query below)

  // Blocks (requires kPermBlocks).
  uint16_t (*registerBlock)(LithoLatticeModHandle mod, const LithoLatticeModBlockDef* def);

  // World (requires kPermWorldRead / kPermWorldWrite).
  uint16_t (*getBlock)(LithoLatticeWorldHandle world, int x, int y, int z);
  int (*setBlock)(LithoLatticeWorldHandle world, int x, int y, int z, uint16_t id);  // 0 = denied

  // Logging (always allowed).
  void (*log)(LithoLatticeModHandle mod, const char* msg);

  // Tick counter (deterministic; no wall-clock in scripts).
  uint64_t (*worldTick)(LithoLatticeWorldHandle world);

  // UI: mods can add UI elements (requires kPermUI).
  void (*registerUiPanel)(LithoLatticeModHandle mod, const char* id, const char* title);
  // Input: mods can register custom actions (requires kPermInput).
  void (*registerInputAction)(LithoLatticeModHandle mod, const char* id, const char* displayName);
  // Assets: mods can load textures/models/sounds (requires kPermAssets).
  int (*loadAsset)(LithoLatticeModHandle mod, const char* path, const char* type);

  // Versioning: mods query this to check compatibility at runtime.
  // Returns e.g. 0x00010000 for 0.1.0 (major << 16 | minor << 8 | patch).
  uint32_t (*engineVersion)(void);
  uint32_t (*sdkVersionFn)(void);  // (was sdkVersion: clashed with the field above)
  uint32_t (*protocolVersion)(void);

  // Network (requires kPermNet). The ONLY network a mod may touch: the engine
  // owns the socket; the mod never sees fds, addresses, or DNS, and there is
  // no socket primitive anywhere else in this struct (audited; see the
  // mod_interface.h audit table in mods_selftest.cpp).
  // Delivery is unreliable, sequenced, latest-wins (see net/channels.h);
  // reliable variants come in a later SDK. `type` is the wire payload type
  // and MUST be >= 256 (net::kModNetTypeBase) — engine traffic kinds (1..4)
  // are refused so mods can never spoof engine traffic.
  // Returns 1 if the datagram was queued, 0 if denied (no permission,
  // throttled by the per-mod rate limiter, or network access suspended).
  // The engine binding routes through net::NetConfinement (origin = Mod),
  // which fails closed on any misconfiguration.
  int (*netSend)(LithoLatticeModHandle mod, uint16_t type, const uint8_t* payload, uint32_t len);
} LithoLatticeModApi;

// Mod lifecycle. The engine loads the mod, then:
//   1. onLoad (get the API)
//   2. onRegister (blocks, components, items)
//   3. onWorldStart / onTick / onWorldStop (per world)
//   4. onUnload
// Half-Life client-DLL style per-frame hooks. The engine calls these; mods hook in.
// Null = not hooked (zero overhead).
typedef struct LithoLatticeMod {
  uint32_t size;
  const char* (*name)(void);
  const char* (*version)(void);
  uint32_t (*permissions)(void);  ///< LithoLatticeModPermission bitmask (for the prompt)
  int (*isFirstParty)(void);  ///< 1 = team mod (extra engine access), 0 = third-party

  // Lifecycle.
  void (*onLoad)(LithoLatticeModHandle mod, const LithoLatticeModApi* api);
  void (*onRegister)(LithoLatticeModHandle mod);
  void (*onWorldStart)(LithoLatticeModHandle mod, LithoLatticeWorldHandle world);
  void (*onWorldStop)(LithoLatticeModHandle mod, LithoLatticeWorldHandle world);
  void (*onUnload)(LithoLatticeModHandle mod);

  // Per-tick (sim). Budget-enforced.
  void (*onTick)(LithoLatticeModHandle mod, LithoLatticeWorldHandle world);

  // Per-frame (client). Called every rendered frame.
  void (*onClientFrame)(LithoLatticeModHandle mod, LithoLatticeWorldHandle world, float dt);
  // Rendering hooks (like HUD_DrawNormalTriangles / HUD_DrawTransparentTriangles).
  void (*onDrawSolid)(LithoLatticeModHandle mod, LithoLatticeWorldHandle world);
  void (*onDrawTranslucent)(LithoLatticeModHandle mod, LithoLatticeWorldHandle world);
  // Input (like HUD_Key_Event). Return 1 to consume.
  int (*onInputEvent)(LithoLatticeModHandle mod, int key, int down);
  // View calculation (like V_CalcRefdef). Mod can override camera.
  void (*onCamera)(LithoLatticeModHandle mod, LithoLatticeWorldHandle world, float* pos, float* angles);
  // Entities (like HUD_AddEntity). Called for each entity before render.
  void (*onEntityVisible)(LithoLatticeModHandle mod, uint32_t entityId, uint32_t type);
  // Inventory changed (for multiplayer sync and mods). Slot 0-41 carry, 64-71 gear.
  void (*onInventoryChanged)(LithoLatticeModHandle mod, uint64_t playerId, uint32_t slot, uint16_t itemId, uint16_t count);
  // Inbound network message, engine -> mod (requires kPermNet). Same
  // unreliable, sequenced, latest-wins semantics as netSend; `type` is the
  // wire payload type (>= 256, never an engine traffic kind). The engine
  // delivers only messages addressed to this mod. Null = not hooked.
  void (*onNetMessage)(LithoLatticeModHandle mod, uint16_t type, const uint8_t* payload, uint32_t len);
} LithoLatticeMod;

#ifdef __cplusplus
}
#endif
