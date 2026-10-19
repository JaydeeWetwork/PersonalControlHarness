// LithoLattice versioning: engine, SDK, and network protocol.
//
// Engine version: semantic (major.minor.patch).
//   Major = breaking changes (save format, network protocol, SDK).
//   Minor = new features (backwards compatible).
//   Patch = bug fixes.
//
// SDK version: the mod_interface.h LITHOLATTICE_SDK_VERSION. Mods declare
//   a compatible range in mod.json: "sdk": ">=1.0.0 <2.0.0".
//
// Protocol version: negotiated at connect. Client and server use the highest
//   common version. New compression/encodings are new protocol versions.
//
// Network tick: the sim runs at 512 TPS, but network snapshots go at 32 Hz
//   (2^5). The client interpolates between snapshots and predicts local input.
//   512 packets/sec would drown in UDP header overhead (28 bytes × 512 = 14KB/s
//   per player just for headers). 32 Hz with delta compression is the sweet spot.
//   Runtime knobs live in SnapshotConfig (snapshot.h) and ReliableConfig
//   (channels.h). The sim is deterministic, so the server sends inputs +
//   deltas, not full state.
// STATUS (F-M8, review 2026-10-13): the sentence above is DESIGN, not
//   shipped behaviour — multiplayer is not implemented (no transport, no
//   server exists in the tree) and anti-cheat is unimplemented (no validator
//   consumes the determinism). See scaling.h's STATUS note.
#pragma once
#include <stdint.h>

#define LITHOLATTICE_ENGINE_VERSION_MAJOR 0
#define LITHOLATTICE_ENGINE_VERSION_MINOR 1
#define LITHOLATTICE_ENGINE_VERSION_PATCH 0

// Network protocol versions. Bump when the wire format changes.
#define LITHOLATTICE_PROTOCOL_V1 1  // baseline: full PlayerState, no compression

#ifdef __cplusplus
namespace litholattice {
/// Engine semver for the connect handshake (see handshake.h). Three packed
/// bytes on the wire.
struct EngineVersion {
  uint8_t major = LITHOLATTICE_ENGINE_VERSION_MAJOR;
  uint8_t minor = LITHOLATTICE_ENGINE_VERSION_MINOR;
  uint8_t patch = LITHOLATTICE_ENGINE_VERSION_PATCH;

  bool operator==(const EngineVersion&) const = default;
};

/// Returns true if the client's protocol version is compatible with the server's.
///
/// Compatibility rule (locked): same major required. Protocol versions are
/// single integers and each one IS its own major — v1/v2 have no minor on
/// the wire — so "same major" is equality. New wire encodings are new
/// protocol versions (never new minors). The negotiation itself
/// (highest mutually compatible) lives in handshake.h.
inline bool protocolCompatible(uint32_t clientProto, uint32_t serverProto) {
  return clientProto == serverProto;
}

/// Returns true if the two engines may talk. The major must match exactly
/// (major = breaking changes: save format, network protocol, SDK).
/// Minor/patch differences within a major are fine (backwards compatible by
/// the semver contract above). Used by the connect handshake.
inline bool engineCompatible(EngineVersion client, EngineVersion server) {
  return client.major == server.major;
}

/// Returns true if the two endpoints' SDK major lines match. The SDK line
/// (LITHOLATTICE_SDK_VERSION, mod_interface.h) is the mod-API breaking
/// line: mods declare a compatible SDK range in mod.json and the engine
/// enforces it at load, and mod traffic (kModNetTypeBase and up) shares the
/// wire — a peer on a different SDK line may frame or route mod traffic
/// differently, so same-line is required and cross-line downgrade is
/// refused (never silently accepted). The net layer treats the line as an
/// opaque u8: the game passes LITHOLATTICE_SDK_VERSION into
/// HandshakeConfig::sdkVersion; kSdkVersionUnset (handshake.h) never
/// survives to a comparison (the handshake fails closed before it).
inline bool sdkCompatible(uint8_t clientSdk, uint8_t serverSdk) {
  return clientSdk == serverSdk;
}

/// Runtime copies of the compile-time version macros. The handshake stamps
/// these into the hello (via HandshakeConfig) instead of re-reading macros
/// at each call site, so a relocated or hot-patched engine reports what it
/// actually runs. (Runtime protocol version: protocol.h's
/// runtimeProtocolVersion(); the SDK line comes from the game's
/// LITHOLATTICE_SDK_VERSION and is exposed to mods through the mod API's
/// sdkVersionFn query.)
inline EngineVersion runtimeEngineVersion() { return EngineVersion{}; }
}  // namespace litholattice
#endif
